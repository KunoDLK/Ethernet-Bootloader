#include "boot_sha1.h"

#include <string.h>

typedef struct
{
  uint32_t state[5];
  uint64_t total_bits;
  uint8_t block[64];
  uint32_t block_used;
} BootSha1Ctx;

static uint32_t rotl32(uint32_t value, uint32_t shift)
{
  return (value << shift) | (value >> (32U - shift));
}

static uint32_t read_be32(const uint8_t bytes[4])
{
  return ((uint32_t)bytes[0] << 24U) |
         ((uint32_t)bytes[1] << 16U) |
         ((uint32_t)bytes[2] << 8U) |
         (uint32_t)bytes[3];
}

static void write_be32(uint8_t bytes[4], uint32_t value)
{
  bytes[0] = (uint8_t)(value >> 24U);
  bytes[1] = (uint8_t)(value >> 16U);
  bytes[2] = (uint8_t)(value >> 8U);
  bytes[3] = (uint8_t)value;
}

static void boot_sha1_init(BootSha1Ctx *ctx)
{
  ctx->state[0] = 0x67452301UL;
  ctx->state[1] = 0xEFCDAB89UL;
  ctx->state[2] = 0x98BADCFEUL;
  ctx->state[3] = 0x10325476UL;
  ctx->state[4] = 0xC3D2E1F0UL;
  ctx->total_bits = 0ULL;
  ctx->block_used = 0U;
}

static void boot_sha1_process_block(BootSha1Ctx *ctx, const uint8_t block[64])
{
  uint32_t w[80];
  for (uint32_t i = 0U; i < 16U; i++)
  {
    w[i] = read_be32(&block[i * 4U]);
  }
  for (uint32_t i = 16U; i < 80U; i++)
  {
    w[i] = rotl32(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
  }

  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  uint32_t e = ctx->state[4];

  for (uint32_t i = 0U; i < 80U; i++)
  {
    uint32_t f;
    uint32_t k;
    if (i < 20U)
    {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999UL;
    }
    else if (i < 40U)
    {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1UL;
    }
    else if (i < 60U)
    {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCUL;
    }
    else
    {
      f = b ^ c ^ d;
      k = 0xCA62C1D6UL;
    }

    const uint32_t temp = rotl32(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl32(b, 30U);
    b = a;
    a = temp;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
}

static void boot_sha1_update(BootSha1Ctx *ctx, const uint8_t *data, size_t length)
{
  if ((data == 0) || (length == 0U))
  {
    return;
  }

  ctx->total_bits += (uint64_t)length * 8ULL;

  while (length != 0U)
  {
    const uint32_t room = 64U - ctx->block_used;
    const uint32_t copy_len = (length < room) ? (uint32_t)length : room;
    memcpy(&ctx->block[ctx->block_used], data, copy_len);
    ctx->block_used += copy_len;
    data += copy_len;
    length -= copy_len;

    if (ctx->block_used == 64U)
    {
      boot_sha1_process_block(ctx, ctx->block);
      ctx->block_used = 0U;
    }
  }
}

static void boot_sha1_final(BootSha1Ctx *ctx, uint8_t digest_out[BOOT_SHA1_DIGEST_BYTES])
{
  ctx->block[ctx->block_used++] = 0x80U;

  if (ctx->block_used > 56U)
  {
    while (ctx->block_used < 64U)
    {
      ctx->block[ctx->block_used++] = 0U;
    }
    boot_sha1_process_block(ctx, ctx->block);
    ctx->block_used = 0U;
  }

  while (ctx->block_used < 56U)
  {
    ctx->block[ctx->block_used++] = 0U;
  }

  for (uint32_t i = 0U; i < 8U; i++)
  {
    ctx->block[56U + i] = (uint8_t)(ctx->total_bits >> ((7U - i) * 8U));
  }
  boot_sha1_process_block(ctx, ctx->block);

  for (uint32_t i = 0U; i < 5U; i++)
  {
    write_be32(&digest_out[i * 4U], ctx->state[i]);
  }
}

void boot_sha1_compute(const void *data, size_t length, uint8_t digest_out[BOOT_SHA1_DIGEST_BYTES])
{
  BootSha1Ctx ctx;
  boot_sha1_init(&ctx);
  boot_sha1_update(&ctx, (const uint8_t *)data, length);
  boot_sha1_final(&ctx, digest_out);
}
