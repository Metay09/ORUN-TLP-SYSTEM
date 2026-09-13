#include "gnss_utc.h"

namespace orun_tlp {
namespace {
constexpr bool leap(uint16_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
constexpr uint8_t kMonthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
}

bool utcToEpoch(const UtcSnapshot& utc, uint32_t& epoch) {
  epoch = 0;
  if (utc.year < 1970 || utc.year > 2106 || utc.month < 1 || utc.month > 12 ||
      utc.hour > 23 || utc.minute > 59 || utc.second > 60) return false;
  const uint8_t days_in_month = kMonthDays[utc.month - 1] +
                               (utc.month == 2 && leap(utc.year) ? 1 : 0);
  if (utc.day < 1 || utc.day > days_in_month) return false;
  uint32_t days = utc.day - 1;
  for (uint16_t year = 1970; year < utc.year; ++year) days += leap(year) ? 366 : 365;
  for (uint8_t month = 1; month < utc.month; ++month)
    days += kMonthDays[month - 1] + (month == 2 && leap(utc.year) ? 1 : 0);
  const uint64_t seconds = uint64_t(days) * 86400 + uint32_t(utc.hour) * 3600 +
                           uint32_t(utc.minute) * 60 + utc.second;
  if (seconds > UINT32_MAX) return false;
  epoch = static_cast<uint32_t>(seconds);
  return true;
}
}  // namespace orun_tlp
