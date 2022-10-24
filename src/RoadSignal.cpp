#include "RoadSignal.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace odr
{

RoadSignal::RoadSignal(std::string road_id,
                       std::string id,
                       double      s,
                       double      t,
                       double      zOffset,
                       double      value,
                       double      height,
                       double      width,
                       double      hOffset,
                       double      pitch,
                       double      roll,
                       std::string name,
                       std::string dynamic,
                       std::string orientation,
                       std::string country,
                       std::string countryRevision,
                       std::string type,
                       std::string subtype,
                       std::string unit,
                       std::string text) :
    road_id(road_id), id(id),
    s(s), t(t), zOffset(zOffset), value(value), height(height), width(width), hOffset(hOffset), pitch(pitch), roll(roll),
    name(name), dynamic(dynamic), orientation(orientation), country(country), countryRevision(countryRevision),
    type(type), subtype(subtype), unit(unit), text(text)
{
}

} // namespace odr
