#include "geofence_staging.h"

namespace orun_tlp {

void GeofenceStaging::begin() {
  active_ = true;
  areas_ = 1;
  total_ = 0;
  for (uint8_t& count : counts_) count = 0;
}

void GeofenceStaging::abort() {
  active_ = false;
  areas_ = 0;
  total_ = 0;
  for (uint8_t& count : counts_) count = 0;
}

GeofenceStaging::Result GeofenceStaging::addVertex(const GeoPointE7& point) {
  if (!active_) return Result::kNotStaging;
  if (total_ >= geofence_format::kMaximumTotalVertices) return Result::kFull;
  vertices_[total_++] = point;
  ++counts_[areas_ - 1];
  return Result::kOk;
}

GeofenceStaging::Result GeofenceStaging::nextArea() {
  if (!active_) return Result::kNotStaging;
  if (counts_[areas_ - 1] == 0) return Result::kEmptyArea;
  if (areas_ >= geofence_format::kMaximumAreas) return Result::kTooManyAreas;
  ++areas_;
  return Result::kOk;
}

uint8_t GeofenceStaging::areaCount() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < areas_; ++i)
    if (counts_[i] != 0) ++count;
  return count;
}

GeofenceAreaSetView GeofenceStaging::view(GeofencePolygonView* polygons) const {
  if (polygons == nullptr) return GeofenceAreaSetView();
  uint8_t out = 0;
  uint16_t offset = 0;
  for (uint8_t i = 0; i < areas_; ++i) {
    if (counts_[i] == 0) continue;
    polygons[out++] = GeofencePolygonView(vertices_ + offset, counts_[i]);
    offset = static_cast<uint16_t>(offset + counts_[i]);
  }
  return GeofenceAreaSetView(polygons, out);
}

bool parseDegreesE7(const char* text, size_t length, int32_t max_abs_degrees,
                    int32_t* value_e7) {
  if (text == nullptr || value_e7 == nullptr || length == 0) return false;
  size_t i = 0;
  const bool negative = text[0] == '-';
  if (negative) ++i;

  int64_t whole = 0;
  size_t whole_digits = 0;
  while (i < length && text[i] >= '0' && text[i] <= '9') {
    whole = whole * 10 + (text[i] - '0');
    if (++whole_digits > 3) return false;
    ++i;
  }
  if (whole_digits == 0) return false;

  int64_t fraction = 0;
  size_t fraction_digits = 0;
  if (i < length && text[i] == '.') {
    ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') {
      fraction = fraction * 10 + (text[i] - '0');
      if (++fraction_digits > 7) return false;
      ++i;
    }
    if (fraction_digits == 0) return false;
  }
  if (i != length) return false;
  for (size_t pad = fraction_digits; pad < 7; ++pad) fraction *= 10;

  const int64_t magnitude = whole * 10000000LL + fraction;
  if (magnitude > static_cast<int64_t>(max_abs_degrees) * 10000000LL)
    return false;
  *value_e7 = static_cast<int32_t>(negative ? -magnitude : magnitude);
  return true;
}

}  // namespace orun_tlp
