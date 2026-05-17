#include "resident_text.h"

#include <stdio.h>
#include <string.h>

void resident_text_format_ipv4(const uint8_t value[4], char *text, size_t text_size)
{
  (void)snprintf(text, text_size, "%u.%u.%u.%u", value[0], value[1], value[2], value[3]);
}

bool resident_text_ipv4_is_unusable_reserved(const uint8_t addr[4])
{
  static const uint8_t k_all_zero[4] = {0U, 0U, 0U, 0U};
  static const uint8_t k_all_one[4] = {255U, 255U, 255U, 255U};

  if (addr == 0)
  {
    return true;
  }

  return (memcmp(addr, k_all_zero, 4U) == 0) || (memcmp(addr, k_all_one, 4U) == 0);
}

static int hex_nibble(uint8_t ch)
{
  if ((ch >= (uint8_t)'0') && (ch <= (uint8_t)'9'))
  {
    return (int)(ch - (uint8_t)'0');
  }

  if ((ch >= (uint8_t)'A') && (ch <= (uint8_t)'F'))
  {
    return (int)(ch - (uint8_t)'A') + 10;
  }

  if ((ch >= (uint8_t)'a') && (ch <= (uint8_t)'f'))
  {
    return (int)(ch - (uint8_t)'a') + 10;
  }

  return -1;
}

bool resident_text_parse_mac(const uint8_t *text, uint16_t text_len, uint8_t value[6])
{
  bool any_nonzero = false;

  if ((text == 0) || (text_len != 17U) || (value == 0))
  {
    return false;
  }

  for (uint8_t i = 0U; i < 6U; i++)
  {
    const uint16_t offset = (uint16_t)i * 3U;
    const int high = hex_nibble(text[offset]);
    const int low = hex_nibble(text[offset + 1U]);
    if ((high < 0) || (low < 0))
    {
      return false;
    }

    value[i] = (uint8_t)(((uint8_t)high << 4U) | (uint8_t)low);
    any_nonzero = any_nonzero || (value[i] != 0U);

    if ((i < 5U) && (text[offset + 2U] != (uint8_t)':'))
    {
      return false;
    }
  }

  return any_nonzero;
}

bool resident_text_parse_ipv4(const uint8_t *text, uint16_t text_len, uint8_t value[4])
{
  uint32_t octet = 0U;
  uint8_t octet_count = 0U;
  bool have_digit = false;

  if ((text == 0) || (text_len == 0U) || (value == 0))
  {
    return false;
  }

  for (uint16_t i = 0U; i < text_len; i++)
  {
    const uint8_t ch = text[i];
    if ((ch >= (uint8_t)'0') && (ch <= (uint8_t)'9'))
    {
      have_digit = true;
      octet = (octet * 10U) + (uint32_t)(ch - (uint8_t)'0');
      if (octet > 255U)
      {
        return false;
      }
    }
    else if (ch == (uint8_t)'.')
    {
      if (!have_digit || (octet_count >= 3U))
      {
        return false;
      }
      value[octet_count++] = (uint8_t)octet;
      octet = 0U;
      have_digit = false;
    }
    else
    {
      return false;
    }
  }

  if (!have_digit || (octet_count != 3U))
  {
    return false;
  }

  value[octet_count] = (uint8_t)octet;
  return true;
}

bool resident_text_parse_u32(const uint8_t *text, uint16_t text_len, uint32_t *value)
{
  uint32_t parsed = 0U;

  if ((text == 0) || (text_len == 0U) || (value == 0))
  {
    return false;
  }

  for (uint16_t i = 0U; i < text_len; i++)
  {
    const uint8_t ch = text[i];
    if ((ch < (uint8_t)'0') || (ch > (uint8_t)'9'))
    {
      return false;
    }

    parsed = (parsed * 10U) + (uint32_t)(ch - (uint8_t)'0');
  }

  *value = parsed;
  return true;
}

static bool text_equals(const uint8_t *text, uint16_t text_len, const char *expected)
{
  const size_t expected_len = strlen(expected);
  return (text != 0) && (text_len == expected_len) &&
         (memcmp(text, expected, expected_len) == 0);
}

static const uint8_t *trim_text(const uint8_t *text, uint16_t *text_len)
{
  uint16_t start = 0U;
  uint16_t end = *text_len;

  while ((start < *text_len) && ((text[start] == (uint8_t)' ') || (text[start] == (uint8_t)'\t')))
  {
    start++;
  }
  while ((end > start) && ((text[end - 1U] == (uint8_t)' ') || (text[end - 1U] == (uint8_t)'\t')))
  {
    end--;
  }

  *text_len = (uint16_t)(end - start);
  return text + start;
}

bool resident_text_parse_rail_mode(const uint8_t *text, uint16_t text_len, ResidentHardwareRailMode *mode)
{
  if ((mode == 0) || (text == 0) || (text_len == 0U))
  {
    return false;
  }

  const uint8_t *trimmed = trim_text(text, &text_len);
  if (text_len == 0U)
  {
    return false;
  }

  if (text_equals(trimmed, text_len, "Enabled"))
  {
    *mode = RESIDENT_HARDWARE_RAIL_MODE_ENABLED;
    return true;
  }

  if (text_equals(trimmed, text_len, "Disabled"))
  {
    *mode = RESIDENT_HARDWARE_RAIL_MODE_DISABLED;
    return true;
  }

  if (text_equals(trimmed, text_len, "Follow Estop"))
  {
    *mode = RESIDENT_HARDWARE_RAIL_MODE_FOLLOW_ESTOP;
    return true;
  }

  return false;
}

bool resident_text_parse_net_dhcp(const uint8_t *text, uint16_t text_len, uint8_t *enabled_out)
{
  if ((enabled_out == 0) || (text == 0) || (text_len == 0U))
  {
    return false;
  }

  const uint8_t *trimmed = trim_text(text, &text_len);
  if (text_len == 0U)
  {
    return false;
  }

  if (text_equals(trimmed, text_len, "Enabled"))
  {
    *enabled_out = 1U;
    return true;
  }

  if (text_equals(trimmed, text_len, "Disabled"))
  {
    *enabled_out = 0U;
    return true;
  }

  if ((text_len == 1U) && (trimmed[0] == (uint8_t)'1'))
  {
    *enabled_out = 1U;
    return true;
  }

  if ((text_len == 1U) && (trimmed[0] == (uint8_t)'0'))
  {
    *enabled_out = 0U;
    return true;
  }

  return false;
}

void resident_text_bytes_to_hex(const uint8_t *bytes, uint16_t byte_len, char *hex_out, size_t hex_cap)
{
  if ((hex_out == 0) || (hex_cap == 0U))
  {
    return;
  }
  hex_out[0] = '\0';

  if (bytes == 0)
  {
    return;
  }

  for (uint16_t i = 0U; (i < byte_len) && ((((size_t)i * 2U) + 2U) <= (hex_cap - 1U)); i++)
  {
    (void)snprintf(hex_out + ((size_t)i * 2U), hex_cap - ((size_t)i * 2U),
                   "%02X", (unsigned int)bytes[i]);
  }
}
