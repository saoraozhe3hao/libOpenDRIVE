#pragma once
#include "Math.hpp"
#include "Mesh.h"
#include "XmlNode.h"

#include <string>
#include <vector>

namespace odr
{

struct RoadSignal : public XmlNode
{
    RoadSignal(std::string road_id,
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
               std::string text);

    std::string road_id = "";
    std::string id = "";

    std::string name = "";
    std::string dynamic = "";
    std::string orientation = "";
    std::string country = "";
    std::string countryRevision = "";
    std::string type = "";
    std::string subtype = "";
    std::string unit = "";
    std::string text = "";

    double s = 0;
    double t = 0;
    double zOffset = 0;
    double value = 0;
    double height = 0;
    double width = 0;
    double hOffset = 0;
    double pitch = 0;
    double roll = 0;
};

} // namespace odr