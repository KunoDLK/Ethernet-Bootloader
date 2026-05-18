#include "resident_program_manager.h"

#include "boot_app_manager.h"
#include "boot_flash.h"
#include "boot_iap.h"
#include "cmsis_os.h"
#include "proto_common.h"
#include "proto_program_tcp.h"
#include "resident_security.h"

#include <stdbool.h>
#include <stdio.h>

#define PROGRAM_DYNAMIC_PORT_FIRST      ((uint16_t)(CONTROL_PORT + 1U))

typedef enum
{
  PROGRAM_REQUEST_NONE = 0,
  PROGRAM_REQUEST_ERASE,
  PROGRAM_REQUEST_STOP,
  PROGRAM_REQUEST_PAUSE,
  PROGRAM_REQUEST_RUN,
  PROGRAM_REQUEST_READY,
  PROGRAM_REQUEST_APPLY_IAP,
} ProgramRequest;

static volatile ResidentProgramState g_state;
static volatile int g_tcp_port;
static volatile ProgramRequest g_pending_request;
static volatile bool g_worker_busy;
static osMutexId_t g_lock;
static osThreadId_t g_worker_thread;
static StaticTask_t g_worker_task_cb;
static StackType_t g_worker_task_stack[384];
static osMutexAttr_t g_lock_attr = {
  .name = "ProgramMgrLock",
};

static void lock_manager(void)
{
  if (g_lock != 0)
  {
    (void)osMutexAcquire(g_lock, osWaitForever);
  }
}

static void unlock_manager(void)
{
  if (g_lock != 0)
  {
    (void)osMutexRelease(g_lock);
  }
}

static void set_state(ResidentProgramState state)
{
  lock_manager();
  g_state = state;
  unlock_manager();
}

static void set_tcp_port(int port)
{
  lock_manager();
  g_tcp_port = port;
  unlock_manager();
}

static int ensure_programming_listener(void)
{
  const int current_port = resident_program_manager_tcp_port();
  if (current_port > 0)
  {
    if (proto_program_tcp_start((uint16_t)current_port) == 0)
    {
      resident_security_set_programming_unlocked(true, 0U);
      (void)printf("ProgramMgr: TCP listener ready on port %u\r\n", (unsigned int)current_port);
      return 0;
    }

    (void)printf("ProgramMgr: cached TCP port %d was stale, reopening\r\n", current_port);
    set_tcp_port(-1);
  }

  for (uint16_t port = PROGRAM_DYNAMIC_PORT_FIRST; port < (PROGRAM_DYNAMIC_PORT_FIRST + 64U); port++)
  {
    if (proto_program_tcp_start(port) == 0)
    {
      set_tcp_port((int)port);
      resident_security_set_programming_unlocked(true, 0U);
      (void)printf("ProgramMgr: TCP listener ready on port %u\r\n", (unsigned int)port);
      return 0;
    }
  }

  set_tcp_port(-1);
  resident_security_set_programming_unlocked(false, 0U);
  (void)printf("ProgramMgr: failed to open TCP listener\r\n");
  return -1;
}

static void close_programming_listener(void)
{
  proto_program_tcp_stop();
  set_tcp_port(-1);
  resident_security_set_programming_unlocked(false, 0U);
}

static int queue_request(ProgramRequest request, ResidentProgramState visible_state)
{
  int result = 0;
  lock_manager();
  if (g_worker_busy || (g_pending_request != PROGRAM_REQUEST_NONE))
  {
    result = -1;
    (void)printf("ProgramMgr: request busy\r\n");
  }
  else
  {
    g_state = visible_state;
    g_pending_request = request;
    (void)printf("ProgramMgr: queued request %d\r\n", (int)request);
  }
  unlock_manager();
  return result;
}

static ProgramRequest take_request(void)
{
  ProgramRequest request;
  lock_manager();
  request = g_pending_request;
  if (request != PROGRAM_REQUEST_NONE)
  {
    g_pending_request = PROGRAM_REQUEST_NONE;
    g_worker_busy = true;
  }
  unlock_manager();
  return request;
}

static void finish_request(void)
{
  lock_manager();
  g_worker_busy = false;
  unlock_manager();
}

static void handle_erase_request(void)
{
  (void)printf("ProgramMgr: erase requested\r\n");
  close_programming_listener();
  (void)boot_app_manager_stop();

  (void)printf("ProgramMgr: erasing app flash\r\n");
  if (boot_flash_erase_app_storage() != 0)
  {
    (void)printf("ProgramMgr: app flash erase failed\r\n");
    set_state(RESIDENT_PROGRAM_STATE_STOPPED);
    finish_request();
    return;
  }

  (void)printf("ProgramMgr: app flash erase complete\r\n");
  if (ensure_programming_listener() != 0)
  {
    set_state(RESIDENT_PROGRAM_STATE_STOPPED);
  }
  else
  {
    set_state(RESIDENT_PROGRAM_STATE_PROGRAMMING_READY);
  }
  finish_request();
}

static void handle_stop_request(void)
{
  (void)printf("ProgramMgr: stop requested\r\n");
  close_programming_listener();
  (void)boot_app_manager_stop();
  set_state(RESIDENT_PROGRAM_STATE_STOPPED);
  finish_request();
}

static void handle_pause_request(void)
{
  (void)printf("ProgramMgr: pause requested\r\n");
  close_programming_listener();
  if (boot_app_manager_pause() == 0)
  {
    set_state(RESIDENT_PROGRAM_STATE_PAUSED);
  }
  finish_request();
}

static void handle_run_request(void)
{
  (void)printf("ProgramMgr: run requested\r\n");
  close_programming_listener();
  if (boot_app_manager_resume() == 0)
  {
    set_state(RESIDENT_PROGRAM_STATE_RUNNING);
  }
  finish_request();
}

static void handle_ready_request(void)
{
  (void)printf("ProgramMgr: ready requested\r\n");
  if (ensure_programming_listener() != 0)
  {
    set_state(RESIDENT_PROGRAM_STATE_STOPPED);
  }
  else
  {
    set_state(RESIDENT_PROGRAM_STATE_PROGRAMMING_READY);
  }
  finish_request();
}

static void handle_apply_iap_request(void)
{
  (void)printf("ProgramMgr: apply boot update requested\r\n");
  close_programming_listener();
  (void)boot_app_manager_stop();
  if (boot_iap_app_start() == 0)
  {
    set_state(RESIDENT_PROGRAM_STATE_RUNNING);
  }
  else
  {
    (void)printf("ProgramMgr: failed to start IAP app\r\n");
    set_state(RESIDENT_PROGRAM_STATE_STOPPED);
  }
  finish_request();
}

static void program_worker(void *argument)
{
  (void)argument;

  for (;;)
  {
    switch (take_request())
    {
      case PROGRAM_REQUEST_ERASE:
        handle_erase_request();
        break;
      case PROGRAM_REQUEST_STOP:
        handle_stop_request();
        break;
      case PROGRAM_REQUEST_PAUSE:
        handle_pause_request();
        break;
      case PROGRAM_REQUEST_RUN:
        handle_run_request();
        break;
      case PROGRAM_REQUEST_READY:
        handle_ready_request();
        break;
      case PROGRAM_REQUEST_APPLY_IAP:
        handle_apply_iap_request();
        break;
      case PROGRAM_REQUEST_NONE:
      default:
        osDelay(10U);
        break;
    }
  }
}

void resident_program_manager_init(void)
{
  g_state = RESIDENT_PROGRAM_STATE_STOPPED;
  g_tcp_port = -1;
  g_pending_request = PROGRAM_REQUEST_NONE;
  g_worker_busy = false;

  g_lock = osMutexNew(&g_lock_attr);

  const osThreadAttr_t task_attr = {
    .name = "ProgramMgr",
    .cb_mem = &g_worker_task_cb,
    .cb_size = sizeof(g_worker_task_cb),
    .stack_mem = g_worker_task_stack,
    .stack_size = sizeof(g_worker_task_stack),
    .priority = osPriorityLow,
  };
  g_worker_thread = osThreadNew(program_worker, 0, &task_attr);
  (void)printf("ProgramMgr: init worker=%p\r\n", (void *)g_worker_thread);
}

int resident_program_manager_request_state(ResidentProgramState state)
{
  switch (state)
  {
    case RESIDENT_PROGRAM_STATE_ERASING:
      return queue_request(PROGRAM_REQUEST_ERASE, RESIDENT_PROGRAM_STATE_ERASING);
    case RESIDENT_PROGRAM_STATE_PROGRAMMING_READY:
      return queue_request(PROGRAM_REQUEST_READY, RESIDENT_PROGRAM_STATE_PROGRAMMING_READY);
    case RESIDENT_PROGRAM_STATE_STOPPED:
      return queue_request(PROGRAM_REQUEST_STOP, RESIDENT_PROGRAM_STATE_STOPPED);
    case RESIDENT_PROGRAM_STATE_PAUSED:
      return queue_request(PROGRAM_REQUEST_PAUSE, RESIDENT_PROGRAM_STATE_PAUSED);
    case RESIDENT_PROGRAM_STATE_RUNNING:
      return queue_request(PROGRAM_REQUEST_RUN, RESIDENT_PROGRAM_STATE_RUNNING);
    default:
      return -1;
  }
}

int resident_program_manager_request_apply_boot_update(void)
{
  return queue_request(PROGRAM_REQUEST_APPLY_IAP, RESIDENT_PROGRAM_STATE_STOPPED);
}

ResidentProgramState resident_program_manager_state(void)
{
  ResidentProgramState state;
  lock_manager();
  state = g_state;
  unlock_manager();
  return state;
}

int resident_program_manager_tcp_port(void)
{
  int port;
  lock_manager();
  port = g_tcp_port;
  unlock_manager();
  return port;
}

void resident_program_manager_mark_stopped(void)
{
  (void)printf("ProgramMgr: marked stopped\r\n");
  close_programming_listener();
  set_state(RESIDENT_PROGRAM_STATE_STOPPED);
}

void resident_program_manager_mark_programming_ready(void)
{
  (void)printf("ProgramMgr: marked programming ready\r\n");
  if (ensure_programming_listener() != 0)
  {
    set_state(RESIDENT_PROGRAM_STATE_STOPPED);
  }
  else
  {
    set_state(RESIDENT_PROGRAM_STATE_PROGRAMMING_READY);
  }
}
