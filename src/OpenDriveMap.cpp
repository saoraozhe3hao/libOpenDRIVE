#include "OpenDriveMap.h"
#include "Geometries/Arc.h"
#include "Geometries/CubicSpline.h"
#include "Geometries/Line.h"
#include "Geometries/ParamPoly3.h"
#include "Geometries/RoadGeometry.h"
#include "Geometries/Spiral.h"
#include "Junction.h"
#include "Lane.h"
#include "LaneSection.h"
#include "Math.hpp"
#include "RefLine.h"
#include "Road.h"
#include "RoadMark.h"
#include "RoadObject.h"
#include "Utils.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <stdio.h>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace odr
{
OpenDriveMap::OpenDriveMap(const std::string& xodr_file, const OpenDriveMapConfig& config) : xodr_file(xodr_file)
{
    pugi::xml_parse_result result = this->xml_doc.load_file(xodr_file.c_str());
    if (!result)
        printf("%s\n", result.description());

    pugi::xml_node odr_node = this->xml_doc.child("OpenDRIVE");

    if (auto geoReference_node = odr_node.child("header").child("geoReference"))
        this->proj4 = geoReference_node.text().as_string("");

    std::size_t cnt = 1;
    if (config.center_map)
    {
        for (pugi::xml_node road_node : odr_node.children("road"))
        {
            for (pugi::xml_node geometry_hdr_node : road_node.child("planView").children("geometry"))
            {
                const double x0 = geometry_hdr_node.attribute("x").as_double(0.0);
                this->x_offs = this->x_offs + ((x0 - this->x_offs) / cnt);
                const double y0 = geometry_hdr_node.attribute("y").as_double(0.0);
                this->y_offs = this->y_offs + ((y0 - this->y_offs) / cnt);
                cnt++;
            }
        }
    }

    for (pugi::xml_node junction_node : odr_node.children("junction"))
    {
        /* make junction */
        const std::string junction_id = junction_node.attribute("id").as_string("");

        Junction& junction =
            this->id_to_junction.insert({junction_id, Junction(junction_node.attribute("name").as_string(""), junction_id)}).first->second;
        junction.xml_node = junction_node;

        for (pugi::xml_node connection_node : junction_node.children("connection"))
        {
            std::string contact_point_str = connection_node.attribute("contactPoint").as_string("");
            CHECK_AND_REPAIR(contact_point_str == "start" || contact_point_str == "end",
                             "Junction::Connection::contactPoint invalid value",
                             contact_point_str = "start"); // default to start
            const JunctionConnection::ContactPoint junction_conn_contact_point =
                (contact_point_str == "start") ? JunctionConnection::ContactPoint_Start : JunctionConnection::ContactPoint_End;

            const std::string   junction_connection_id = connection_node.attribute("id").as_string("");
            JunctionConnection& junction_connection = junction.id_to_connection
                                                          .insert({junction_connection_id,
                                                                   JunctionConnection(junction_connection_id,
                                                                                      connection_node.attribute("incomingRoad").as_string(""),
                                                                                      connection_node.attribute("connectingRoad").as_string(""),
                                                                                      junction_conn_contact_point)})
                                                          .first->second;

            for (pugi::xml_node lane_link_node : connection_node.children("laneLink"))
            {
                JunctionLaneLink lane_link(lane_link_node.attribute("from").as_int(0), lane_link_node.attribute("to").as_int(0));
                junction_connection.lane_links.insert(lane_link);
            }
        }

        const std::size_t num_conns = junction.id_to_connection.size();
        CHECK(num_conns > 0, "Junction::connections == 0");
        if (num_conns < 1)
            continue;

        for (pugi::xml_node priority_node : junction_node.children("priority"))
        {
            JunctionPriority junction_priority(priority_node.attribute("high").as_string(""), priority_node.attribute("low").as_string(""));
            junction.priorities.insert(junction_priority);
        }

        for (pugi::xml_node controller_node : junction_node.children("controller"))
        {
            const std::string junction_controller_id = controller_node.attribute("id").as_string("");
            junction.id_to_controller.insert({junction_controller_id,
                                              JunctionController(junction_controller_id,
                                                                 controller_node.attribute("type").as_string(""),
                                                                 controller_node.attribute("sequence").as_uint(0))});
        }
    }

    for (pugi::xml_node road_node : odr_node.children("road"))
    {
        /* make road */
        const std::string road_id = road_node.attribute("id").as_string("");

        Road& road = this->id_to_road
                         .insert({road_id,
                                  Road(road_id,
                                       road_node.attribute("length").as_double(0.0),
                                       road_node.attribute("junction").as_string(""),
                                       road_node.attribute("name").as_string(""))})
                         .first->second;
        road.xml_node = road_node;

        CHECK_AND_REPAIR(road.length >= 0, "road::length < 0", road.length = 0);

        /* parse road links */
        for (bool is_predecessor : {true, false})
        {
            pugi::xml_node road_link_node =
                is_predecessor ? road_node.child("link").child("predecessor") : road_node.child("link").child("successor");
            if (road_link_node)
            {
                RoadLink& link = is_predecessor ? road.predecessor : road.successor;
                link.id = road_link_node.attribute("elementId").as_string("");

                std::string type_str = road_link_node.attribute("elementType").as_string("");
                CHECK_AND_REPAIR(type_str == "road" || type_str == "junction",
                                 "Road::Succ/Predecessor::Link::elementType invalid type",
                                 type_str = "road"); // default to road
                link.type = (type_str == "road") ? RoadLink::Type_Road : RoadLink::Type_Junction;

                if (link.type == RoadLink::Type_Road)
                {
                    // junction connection has no contact point
                    std::string contact_point_str = road_link_node.attribute("contactPoint").as_string("");
                    CHECK_AND_REPAIR(contact_point_str == "start" || contact_point_str == "end",
                                     "Road::Succ/Predecessor::Link::contactPoint invalid type",
                                     contact_point_str = "start"); // default to start
                    link.contact_point = (contact_point_str == "start") ? RoadLink::ContactPoint_Start : RoadLink::ContactPoint_End;
                }

                link.xml_node = road_link_node;
            }
        }

        /* parse road neighbors */
        for (pugi::xml_node road_neighbor_node : road_node.child("link").children("neighbor"))
        {
            const std::string road_neighbor_id = road_neighbor_node.attribute("elementId").as_string("");
            const std::string road_neighbor_side = road_neighbor_node.attribute("side").as_string("");
            const std::string road_neighbor_direction = road_neighbor_node.attribute("direction").as_string("");
            RoadNeighbor      road_neighbor(road_neighbor_id, road_neighbor_side, road_neighbor_direction);
            road_neighbor.xml_node = road_neighbor_node;
            road.neighbors.push_back(road_neighbor);
        }

        /* parse road type and speed */
        for (pugi::xml_node road_type_node : road_node.children("type"))
        {
            double      s = road_type_node.attribute("s").as_double(0.0);
            std::string type = road_type_node.attribute("type").as_string("");

            CHECK_AND_REPAIR(s >= 0, "road::type::s < 0", s = 0);

            road.s_to_type[s] = type;
            if (pugi::xml_node node = road_type_node.child("speed"))
            {
                const std::string speed_record_max = node.attribute("max").as_string("");
                const std::string speed_record_unit = node.attribute("unit").as_string("");
                SpeedRecord       speed_record(speed_record_max, speed_record_unit);
                speed_record.xml_node = node;
                road.s_to_speed.insert({s, speed_record});
            }
        }

        /* make ref_line - parse road geometries */
        for (pugi::xml_node geometry_hdr_node : road_node.child("planView").children("geometry"))
        {
            double s0 = geometry_hdr_node.attribute("s").as_double(0.0);
            double x0 = geometry_hdr_node.attribute("x").as_double(0.0) - this->x_offs;
            double y0 = geometry_hdr_node.attribute("y").as_double(0.0) - this->y_offs;
            double hdg0 = geometry_hdr_node.attribute("hdg").as_double(0.0);
            double length = geometry_hdr_node.attribute("length").as_double(0.0);

            CHECK_AND_REPAIR(s0 >= 0, "road::planView::geometry::s < 0", s0 = 0);
            CHECK_AND_REPAIR(length >= 0, "road::planView::geometry::length < 0", length = 0);

            pugi::xml_node geometry_node = geometry_hdr_node.first_child();
            std::string    geometry_type = geometry_node.name();
            if (geometry_type == "line")
            {
                road.ref_line.s0_to_geometry[s0] = std::make_unique<Line>(s0, x0, y0, hdg0, length);
            }
            else if (geometry_type == "spiral")
            {
                double curv_start = geometry_node.attribute("curvStart").as_double(0.0);
                double curv_end = geometry_node.attribute("curvEnd").as_double(0.0);
                road.ref_line.s0_to_geometry[s0] = std::make_unique<Spiral>(s0, x0, y0, hdg0, length, curv_start, curv_end);
            }
            else if (geometry_type == "arc")
            {
                double curvature = geometry_node.attribute("curvature").as_double(0.0);
                road.ref_line.s0_to_geometry[s0] = std::make_unique<Arc>(s0, x0, y0, hdg0, length, curvature);
            }
            else if (geometry_type == "paramPoly3")
            {
                double aU = geometry_node.attribute("aU").as_double(0.0);
                double bU = geometry_node.attribute("bU").as_double(0.0);
                double cU = geometry_node.attribute("cU").as_double(0.0);
                double dU = geometry_node.attribute("dU").as_double(0.0);
                double aV = geometry_node.attribute("aV").as_double(0.0);
                double bV = geometry_node.attribute("bV").as_double(0.0);
                double cV = geometry_node.attribute("cV").as_double(0.0);
                double dV = geometry_node.attribute("dV").as_double(0.0);

                bool pRange_normalized = true;
                if (geometry_node.attribute("pRange") || geometry_hdr_node.attribute("pRange"))
                {
                    std::string pRange_str = geometry_node.attribute("pRange") ? geometry_node.attribute("pRange").as_string("")
                                                                               : geometry_hdr_node.attribute("pRange").as_string("");
                    std::transform(pRange_str.begin(), pRange_str.end(), pRange_str.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (pRange_str == "arclength")
                        pRange_normalized = false;
                }
                road.ref_line.s0_to_geometry[s0] =
                    std::make_unique<ParamPoly3>(s0, x0, y0, hdg0, length, aU, bU, cU, dU, aV, bV, cV, dV, pRange_normalized);
            }
            else
            {
                printf("Could not parse %s\n", geometry_type.c_str());
                continue;
            }

            road.ref_line.s0_to_geometry.at(s0)->xml_node = geometry_node;
        }

        std::map<std::string /*x path query*/, CubicSpline&> cubic_spline_fields{{".//elevationProfile//elevation", road.ref_line.elevation_profile},
                                                                                 {".//lanes//laneOffset", road.lane_offset}};

        if (config.with_lateralProfile)
            cubic_spline_fields.insert({".//lateralProfile//superelevation", road.superelevation});

        /* parse elevation profiles, lane offsets, superelevation */
        for (auto entry : cubic_spline_fields)
        {
            pugi::xpath_node_set nodes = road_node.select_nodes(entry.first.c_str());
            for (pugi::xpath_node node : nodes)
            {
                double s0 = node.node().attribute("s").as_double(0.0);
                double a = node.node().attribute("a").as_double(0.0);
                double b = node.node().attribute("b").as_double(0.0);
                double c = node.node().attribute("c").as_double(0.0);
                double d = node.node().attribute("d").as_double(0.0);

                CHECK_AND_REPAIR(s0 >= 0, (entry.first + "::s < 0").c_str(), s0 = 0);

                entry.second.s0_to_poly[s0] = Poly3(s0, a, b, c, d);
            }
        }

        /* parse crossfall - has extra attribute side */
        if (config.with_lateralProfile)
        {
            for (pugi::xml_node crossfall_node : road_node.child("lateralProfile").children("crossfall"))
            {
                double s0 = crossfall_node.attribute("s").as_double(0.0);
                double a = crossfall_node.attribute("a").as_double(0.0);
                double b = crossfall_node.attribute("b").as_double(0.0);
                double c = crossfall_node.attribute("c").as_double(0.0);
                double d = crossfall_node.attribute("d").as_double(0.0);

                CHECK_AND_REPAIR(s0 >= 0, "road::lateralProfile::crossfall::s < 0", s0 = 0);

                Poly3 crossfall_poly(s0, a, b, c, d);
                road.crossfall.s0_to_poly[s0] = crossfall_poly;
                if (pugi::xml_attribute side = crossfall_node.attribute("side"))
                {
                    std::string side_str = side.as_string("");
                    std::transform(side_str.begin(), side_str.end(), side_str.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (side_str == "left")
                        road.crossfall.sides[s0] = Crossfall::Side_Left;
                    else if (side_str == "right")
                        road.crossfall.sides[s0] = Crossfall::Side_Right;
                    else
                        road.crossfall.sides[s0] = Crossfall::Side_Both;
                }
            }

            /* check for lateralProfile shape - not implemented yet */
            for (auto road_shape_node : road_node.child("lateralProfile").children("shape"))
                printf("Lateral Profile Shape not supported\n");
        }

        /* parse road lane sections and lanes */
        for (pugi::xml_node lanesection_node : road_node.child("lanes").children("laneSection"))
        {
            const double s0 = lanesection_node.attribute("s").as_double(0.0);
            LaneSection& lanesection = road.s_to_lanesection.insert({s0, LaneSection(road_id, s0)}).first->second;
            lanesection.xml_node = lanesection_node;

            for (pugi::xpath_node lane_xpath_node : lanesection_node.select_nodes(".//lane"))
            {
                pugi::xml_node lane_node = lane_xpath_node.node();
                const int      lane_id = lane_node.attribute("id").as_int(0);

                Lane& lane =
                    lanesection.id_to_lane
                        .insert({lane_id,
                                 Lane(road_id, s0, lane_id, lane_node.attribute("level").as_bool(false), lane_node.attribute("type").as_string(""))})
                        .first->second;

                if (pugi::xml_node node = lane_node.child("link").child("predecessor"))
                    lane.predecessor = node.attribute("id").as_int(0);
                if (pugi::xml_node node = lane_node.child("link").child("successor"))
                    lane.successor = node.attribute("id").as_int(0);
                lane.xml_node = lane_node;

                for (pugi::xml_node lane_width_node : lane_node.children("width"))
                {
                    double s_offset = lane_width_node.attribute("sOffset").as_double(0.0);
                    double a = lane_width_node.attribute("a").as_double(0.0);
                    double b = lane_width_node.attribute("b").as_double(0.0);
                    double c = lane_width_node.attribute("c").as_double(0.0);
                    double d = lane_width_node.attribute("d").as_double(0.0);

                    CHECK_AND_REPAIR(s_offset >= 0, "lane::width::sOffset < 0", s_offset = 0);
                    lane.lane_width.s0_to_poly[s0 + s_offset] = Poly3(s0 + s_offset, a, b, c, d);
                }

                if (config.with_laneHeight)
                {
                    for (pugi::xml_node lane_height_node : lane_node.children("height"))
                    {
                        double s_offset = lane_height_node.attribute("sOffset").as_double(0.0);
                        double inner = lane_height_node.attribute("inner").as_double(0.0);
                        double outer = lane_height_node.attribute("outer").as_double(0.0);

                        CHECK_AND_REPAIR(s_offset >= 0, "lane::height::sOffset < 0", s_offset = 0);
                        lane.s_to_height_offset.insert({s0 + s_offset, HeightOffset(inner, outer)});
                    }
                }

                for (pugi::xml_node roadmark_node : lane_node.children("roadMark"))
                {
                    RoadMarkGroup roadmark_group(road_id,
                                                 s0,
                                                 lane_id,
                                                 roadmark_node.attribute("width").as_double(-1),
                                                 roadmark_node.attribute("height").as_double(0),
                                                 roadmark_node.attribute("sOffset").as_double(0),
                                                 roadmark_node.attribute("type").as_string("none"),
                                                 roadmark_node.attribute("weight").as_string("standard"),
                                                 roadmark_node.attribute("color").as_string("standard"),
                                                 roadmark_node.attribute("material").as_string("standard"),
                                                 roadmark_node.attribute("laneChange").as_string("both"));
                    roadmark_group.xml_node = roadmark_node;

                    CHECK_AND_REPAIR(roadmark_group.s_offset >= 0, "lane::roadMark::sOffset < 0", roadmark_group.s_offset = 0);
                    const double roadmark_group_s0 = s0 + roadmark_group.s_offset;

                    if (pugi::xml_node roadmark_type_node = roadmark_node.child("type"))
                    {
                        const std::string name = roadmark_type_node.attribute("name").as_string("");
                        const double      line_width_1 = roadmark_type_node.attribute("width").as_double(-1);

                        for (pugi::xml_node roadmarks_line_node : roadmark_type_node.children("line"))
                        {
                            const double line_width_0 = roadmarks_line_node.attribute("width").as_double(-1);
                            const double roadmark_width = line_width_0 < 0 ? line_width_1 : line_width_0;

                            RoadMarksLine roadmarks_line(road_id,
                                                         s0,
                                                         lane_id,
                                                         roadmark_group_s0,
                                                         roadmark_width,
                                                         roadmarks_line_node.attribute("length").as_double(0),
                                                         roadmarks_line_node.attribute("space").as_double(0),
                                                         roadmarks_line_node.attribute("tOffset").as_double(0),
                                                         roadmarks_line_node.attribute("sOffset").as_double(0),
                                                         name,
                                                         roadmarks_line_node.attribute("rule").as_string("none"));
                            roadmarks_line.xml_node = roadmarks_line_node;

                            CHECK_AND_REPAIR(roadmarks_line.length >= 0, "roadMark::type::line::length < 0", roadmarks_line.length = 0);
                            CHECK_AND_REPAIR(roadmarks_line.space >= 0, "roadMark::type::line::space < 0", roadmarks_line.space = 0);
                            CHECK_AND_REPAIR(roadmarks_line.s_offset >= 0, "roadMark::type::line::sOffset < 0", roadmarks_line.s_offset = 0);

                            roadmark_group.roadmark_lines.emplace(std::move(roadmarks_line));
                        }
                    }

                    lane.roadmark_groups.emplace(std::move(roadmark_group));
                }
            }

            /* derive lane borders from lane widths */
            auto id_lane_iter0 = lanesection.id_to_lane.find(0);
            if (id_lane_iter0 == lanesection.id_to_lane.end())
                throw std::runtime_error("lane section does not have lane #0");

            /* iterate from id #0 towards +inf */
            auto id_lane_iter1 = std::next(id_lane_iter0);
            for (auto iter = id_lane_iter1; iter != lanesection.id_to_lane.end(); iter++)
            {
                if (iter == id_lane_iter0)
                {
                    iter->second.outer_border = iter->second.lane_width;
                }
                else
                {
                    iter->second.inner_border = std::prev(iter)->second.outer_border;
                    iter->second.outer_border = std::prev(iter)->second.outer_border.add(iter->second.lane_width);
                }
            }

            /* iterate from id #0 towards -inf */
            std::map<int, Lane>::reverse_iterator r_id_lane_iter_1(id_lane_iter0);
            for (auto r_iter = r_id_lane_iter_1; r_iter != lanesection.id_to_lane.rend(); r_iter++)
            {
                if (r_iter == r_id_lane_iter_1)
                {
                    r_iter->second.outer_border = r_iter->second.lane_width.negate();
                }
                else
                {
                    r_iter->second.inner_border = std::prev(r_iter)->second.outer_border;
                    r_iter->second.outer_border = std::prev(r_iter)->second.outer_border.add(r_iter->second.lane_width.negate());
                }
            }

            for (auto& id_lane : lanesection.id_to_lane)
            {
                id_lane.second.inner_border = id_lane.second.inner_border.add(road.lane_offset);
                id_lane.second.outer_border = id_lane.second.outer_border.add(road.lane_offset);
            }
        }

        /* parse road objects */
        if (config.with_road_objects)
        {
            const RoadObjectCorner::Type default_local_outline_type =
                config.abs_z_for_for_local_road_obj_outline ? RoadObjectCorner::Type_Local_AbsZ : RoadObjectCorner::Type_Local_RelZ;

            for (pugi::xml_node object_node : road_node.child("objects").children("object"))
            {
                std::string road_object_id = object_node.attribute("id").as_string("");
                CHECK_AND_REPAIR(road.id_to_object.find(road_object_id) == road.id_to_object.end(),
                                 (std::string("object::id already exists - ") + road_object_id).c_str(),
                                 road_object_id = road_object_id + std::string("_dup"));

                RoadObject& road_object = road.id_to_object
                                              .insert({road_object_id,
                                                       RoadObject(road_id,
                                                                  road_object_id,
                                                                  object_node.attribute("s").as_double(0),
                                                                  object_node.attribute("t").as_double(0),
                                                                  object_node.attribute("zOffset").as_double(0),
                                                                  object_node.attribute("length").as_double(0),
                                                                  object_node.attribute("validLength").as_double(0),
                                                                  object_node.attribute("width").as_double(0),
                                                                  object_node.attribute("radius").as_double(0),
                                                                  object_node.attribute("height").as_double(0),
                                                                  object_node.attribute("hdg").as_double(0),
                                                                  object_node.attribute("pitch").as_double(0),
                                                                  object_node.attribute("roll").as_double(0),
                                                                  object_node.attribute("type").as_string(""),
                                                                  object_node.attribute("name").as_string(""),
                                                                  object_node.attribute("orientation").as_string(""))})
                                              .first->second;
                road_object.xml_node = object_node;

                CHECK_AND_REPAIR(road_object.s0 >= 0, "object::s < 0", road_object.s0 = 0);
                CHECK_AND_REPAIR(road_object.valid_length >= 0, "object::validLength < 0", road_object.valid_length = 0);
                CHECK_AND_REPAIR(road_object.length >= 0, "object::length < 0", road_object.length = 0);
                CHECK_AND_REPAIR(road_object.width >= 0, "object::width < 0", road_object.width = 0);
                CHECK_AND_REPAIR(road_object.radius >= 0, "object::radius < 0", road_object.radius = 0);

                for (pugi::xml_node repeat_node : object_node.children("repeat"))
                {
                    RoadObjectRepeat road_object_repeat(repeat_node.attribute("s").as_double(NAN),
                                                        repeat_node.attribute("length").as_double(0),
                                                        repeat_node.attribute("distance").as_double(0),
                                                        repeat_node.attribute("tStart").as_double(NAN),
                                                        repeat_node.attribute("tEnd").as_double(NAN),
                                                        repeat_node.attribute("widthStart").as_double(NAN),
                                                        repeat_node.attribute("widthEnd").as_double(NAN),
                                                        repeat_node.attribute("heightStart").as_double(NAN),
                                                        repeat_node.attribute("heightEnd").as_double(NAN),
                                                        repeat_node.attribute("zOffsetStart").as_double(NAN),
                                                        repeat_node.attribute("zOffsetEnd").as_double(NAN));
                    road_object_repeat.xml_node = repeat_node;

                    CHECK_AND_REPAIR(
                        std::isnan(road_object_repeat.s0) || road_object_repeat.s0 >= 0, "object::repeat::s < 0", road_object_repeat.s0 = 0);
                    CHECK_AND_REPAIR(std::isnan(road_object_repeat.width_start) || road_object_repeat.width_start >= 0,
                                     "object::repeat::widthStart < 0",
                                     road_object_repeat.width_start = 0);
                    CHECK_AND_REPAIR(std::isnan(road_object_repeat.width_end) || road_object_repeat.width_end >= 0,
                                     "object::repeat::widthStart < 0",
                                     road_object_repeat.width_end = 0);
                    CHECK_AND_REPAIR(road_object_repeat.length >= 0, "object::repeat::length < 0", road_object_repeat.length = 0);
                    CHECK_AND_REPAIR(road_object_repeat.distance >= 0, "object::repeat::distance < 0", road_object_repeat.distance = 0);

                    road_object.repeats.push_back(road_object_repeat);
                }

                for (pugi::xml_node corner_local_node : object_node.child("outline").children("cornerLocal"))
                {
                    const Vec3D pt_local{corner_local_node.attribute("u").as_double(0),
                                         corner_local_node.attribute("v").as_double(0),
                                         corner_local_node.attribute("z").as_double(0)};

                    RoadObjectCorner road_object_corner_local(
                        pt_local, corner_local_node.attribute("height").as_double(0), default_local_outline_type);
                    road_object_corner_local.xml_node = corner_local_node;
                    road_object.outline.push_back(road_object_corner_local);
                }

                for (pugi::xml_node corner_road_node : object_node.child("outline").children("cornerRoad"))
                {
                    const Vec3D pt_road{corner_road_node.attribute("s").as_double(0),
                                        corner_road_node.attribute("t").as_double(0),
                                        corner_road_node.attribute("dz").as_double(0)};

                    RoadObjectCorner road_object_corner_road(pt_road, corner_road_node.attribute("height").as_double(0), RoadObjectCorner::Type_Road);
                    road_object_corner_road.xml_node = corner_road_node;
                    road_object.outline.push_back(road_object_corner_road);
                }
            }
        }
    }
}

std::vector<Road> OpenDriveMap::get_roads() const { return get_map_values(this->id_to_road); }

Vec3D OpenDriveMap::get_xyz(std::string road_id, double s, double t, double h) const {
    return this->id_to_road.at(road_id).get_xyz(s, t, h);
}

double OpenDriveMap::get_section_s0(std::string road_id, double s) const {
    return this->id_to_road.at(road_id).get_lanesection_s0(s);
}

int OpenDriveMap::get_lane_id(std::string road_id, double s, double t) const {
    return this->id_to_road.at(road_id).get_lanesection(s).get_lane_id(s, t);
}

double OpenDriveMap::get_lane_offset(std::string road_id, double s, double t) const {
    LaneSection section = this->id_to_road.at(road_id).get_lanesection(s);
    Lane lane = section.get_lane(s, t);
    int side = t > 0 ? 1 : -1; // 沿着S轴，左侧为正，右侧为负
    double pre_lane_width = 0;    // 内侧几条车道的宽度

    for(int i=1;i<lane.id*side;i++){
        std::map<int, Lane>::iterator it = section.id_to_lane.find(i*side);
        if(it != section.id_to_lane.end()){
            pre_lane_width += it->second.lane_width.get(s);
        }
    }
    double lane_width = lane.lane_width.get(s);
    double offset = (t*side - pre_lane_width - lane_width/2)*side;  // 沿着S轴，左侧为正，右侧为负
    return offset;
}

std::string OpenDriveMap::get_lane_type(std::string road_id, double s, int lane_id) const {
    return this->id_to_road.at(road_id).get_lanesection(s).id_to_lane.at(lane_id).type;
}

bool OpenDriveMap::has_lane_id(std::string road_id, double s, int lane_id) const {
    std::map<int, Lane> id_to_lane = this->id_to_road.at(road_id).get_lanesection(s).id_to_lane;
    return id_to_lane.find(lane_id) != id_to_lane.end();
}

double OpenDriveMap::get_road_t(std::string road_id, double s, int lane_id, double offset) const {
    LaneSection section = this->id_to_road.at(road_id).get_lanesection(s);
    Lane lane = section.id_to_lane.at(lane_id);
    int side = lane_id > 0 ? 1 : -1; // 沿着S轴，左侧为正，右侧为负
    double pre_lane_width = 0;    // 内侧几条车道的宽度

    for(int i=1;i<lane.id*side;i++){
        std::map<int, Lane>::iterator it = section.id_to_lane.find(i*side);
        if(it != section.id_to_lane.end()){
            pre_lane_width += it->second.lane_width.get(s);
        }
    }
    double lane_width = lane.lane_width.get(s);
    double t = (pre_lane_width + lane_width/2 + offset*side)*side;  // 沿着S轴，左侧为正，右侧为负
    return t;
}

double OpenDriveMap::get_road_length(std::string road_id) const {
    return this->id_to_road.at(road_id).length;
}

bool OpenDriveMap::has_road_id(std::string road_id) const {
    return this->id_to_road.find(road_id) != this->id_to_road.end();
}

std::vector<Junction> OpenDriveMap::get_junctions() const { return get_map_values(this->id_to_junction); }

RoutingGraph OpenDriveMap::get_routing_graph() const
{
    RoutingGraph routing_graph;

    /* find lane successors/predecessors */
    for (const bool find_successor : {true, false})
    {
        for (const auto& id_road : this->id_to_road)
        {
            const Road&     road = id_road.second;
            const RoadLink& road_link = find_successor ? road.successor : road.predecessor;
            if (road_link.type != RoadLink::Type_Road || road_link.contact_point == RoadLink::ContactPoint_None)
                continue;

            auto next_road_iter = this->id_to_road.find(road_link.id);
            if (next_road_iter == this->id_to_road.end())
                continue;
            const Road&        next_road = next_road_iter->second;
            const LaneSection& next_road_contact_lanesec = (road_link.contact_point == RoadLink::ContactPoint_Start)
                                                               ? next_road.s_to_lanesection.begin()->second
                                                               : next_road.s_to_lanesection.rbegin()->second;

            for (auto s_lanesec_iter = road.s_to_lanesection.begin(); s_lanesec_iter != road.s_to_lanesection.end(); s_lanesec_iter++)
            {
                const LaneSection& lanesec = s_lanesec_iter->second;
                const LaneSection* next_lanesec = nullptr;
                const Road*        next_lanesecs_road = nullptr;

                if (find_successor && std::next(s_lanesec_iter) == road.s_to_lanesection.end())
                {
                    next_lanesec = &next_road_contact_lanesec; // take next road to find successor
                    next_lanesecs_road = &next_road;
                }
                else if (!find_successor && s_lanesec_iter == road.s_to_lanesection.begin())
                {
                    next_lanesec = &next_road_contact_lanesec; // take prev. road to find predecessor
                    next_lanesecs_road = &next_road;
                }
                else
                {
                    next_lanesec = find_successor ? &(std::next(s_lanesec_iter)->second) : &(std::prev(s_lanesec_iter)->second);
                    next_lanesecs_road = &road;
                }

                for (const auto& id_lane : lanesec.id_to_lane)
                {
                    const Lane& lane = id_lane.second;
                    const int   next_lane_id = find_successor ? lane.successor : lane.predecessor;
                    if (next_lane_id == 0)
                        continue;

                    auto next_lane_iter = next_lanesec->id_to_lane.find(next_lane_id);
                    if (next_lane_iter == next_lanesec->id_to_lane.end())
                        continue;
                    const Lane& next_lane = next_lane_iter->second;

                    const Lane&        from_lane = find_successor ? lane : next_lane;
                    const LaneSection& from_lanesection = find_successor ? lanesec : *next_lanesec;
                    const Road&        from_road = find_successor ? road : *next_lanesecs_road;

                    const Lane&        to_lane = find_successor ? next_lane : lane;
                    const LaneSection& to_lanesection = find_successor ? *next_lanesec : lanesec;
                    const Road&        to_road = find_successor ? *next_lanesecs_road : road;

                    const LaneKey from(from_road.id, from_lanesection.s0, from_lane.id);
                    const LaneKey to(to_road.id, to_lanesection.s0, to_lane.id);
                    const double  lane_length = road.get_lanesection_length(from_lanesection);
                    routing_graph.add_edge(RoutingGraphEdge(from, to, lane_length));
                }
            }
        }
    }

    /* parse junctions */
    for (const auto& id_junc : this->id_to_junction)
    {
        for (const auto& id_conn : id_junc.second.id_to_connection)
        {
            const JunctionConnection& conn = id_conn.second;

            auto incoming_road_iter = this->id_to_road.find(conn.incoming_road);
            auto connecting_road_iter = this->id_to_road.find(conn.connecting_road);
            if (incoming_road_iter == this->id_to_road.end() || connecting_road_iter == this->id_to_road.end())
                continue;
            const Road& incoming_road = incoming_road_iter->second;
            const Road& connecting_road = connecting_road_iter->second;

            const bool is_succ_junc = incoming_road.successor.type == RoadLink::Type_Junction && incoming_road.successor.id == id_junc.first;
            const bool is_pred_junc = incoming_road.predecessor.type == RoadLink::Type_Junction && incoming_road.predecessor.id == id_junc.first;
            if (!is_succ_junc && !is_pred_junc)
                continue;

            const LaneSection& incoming_lanesec =
                is_succ_junc ? incoming_road.s_to_lanesection.rbegin()->second : incoming_road.s_to_lanesection.begin()->second;
            const LaneSection& connecting_lanesec = (conn.contact_point == JunctionConnection::ContactPoint_Start)
                                                        ? connecting_road.s_to_lanesection.begin()->second
                                                        : connecting_road.s_to_lanesection.rbegin()->second;
            for (const JunctionLaneLink& lane_link : conn.lane_links)
            {
                if (lane_link.from == 0 || lane_link.to == 0)
                    continue;
                auto from_lane_iter = incoming_lanesec.id_to_lane.find(lane_link.from);
                auto to_lane_iter = connecting_lanesec.id_to_lane.find(lane_link.to);
                if (from_lane_iter == incoming_lanesec.id_to_lane.end() || to_lane_iter == connecting_lanesec.id_to_lane.end())
                    continue;
                const Lane& from_lane = from_lane_iter->second;
                const Lane& to_lane = to_lane_iter->second;

                const LaneKey from(incoming_road.id, incoming_lanesec.s0, from_lane.id);
                const LaneKey to(connecting_road.id, connecting_lanesec.s0, to_lane.id);
                const double  lane_length = incoming_road.get_lanesection_length(incoming_lanesec);
                routing_graph.add_edge(RoutingGraphEdge(from, to, lane_length));
            }
        }
    }

    return routing_graph;
}

} // namespace odr
