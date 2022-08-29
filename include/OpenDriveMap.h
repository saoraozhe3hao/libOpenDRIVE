#pragma once
#include "Junction.h"
#include "Road.h"
#include "RoutingGraph.h"

#include <pugixml/pugixml.hpp>

#include <map>
#include <string>
#include <vector>

namespace odr
{

struct OpenDriveMapConfig
{
    bool with_lateralProfile = true;
    bool with_laneHeight = true;
    bool with_road_objects = true;
    bool center_map = true;
    bool abs_z_for_for_local_road_obj_outline = false;
};

class OpenDriveMap
{
public:
    OpenDriveMap(const std::string& xodr_file, const OpenDriveMapConfig& config = OpenDriveMapConfig{});

    std::vector<Road>     get_roads() const;
    std::vector<Junction> get_junctions() const;
    Vec3D  get_xyz(std::string road_id, double s, double t, double h) const;
    double get_section_s0(std::string road_id, double s) const;
    int    get_lane_id(std::string road_id, double s, double t) const;
    double get_lane_offset(std::string road_id, double s, double t) const;
    std::string get_lane_type(std::string road_id, double s, int lane_id) const;
    double get_road_t(std::string road_id, double s, int lane_id, double offset) const;
    double get_road_length(std::string road_id) const;

    RoutingGraph get_routing_graph() const;

    std::string        proj4 = "";
    double             x_offs = 0;
    double             y_offs = 0;
    const std::string  xodr_file = "";
    pugi::xml_document xml_doc;

    std::map<std::string, Road>     id_to_road;
    std::map<std::string, Junction> id_to_junction;
};

} // namespace odr