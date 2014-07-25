// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "geo/distances.hpp"

#include "geo/ellipsoid.hpp"
#include "geo/exceptions.hpp"
#include "geo/geojson.hpp"
#include "geo/geo_visitor.hpp"
#include "geo/karney/geodesic.h"
#include "geo/s2/s2.h"
#include "geo/s2/s2latlng.h"
#include "geo/s2/s2polygon.h"
#include "geo/s2/s2polyline.h"

double geodesic_distance(const lat_lon_point_t &p1,
                         const lat_lon_point_t &p2,
                         const ellipsoid_spec_t &e) {
    // Use Karney's algorithm
    struct geod_geodesic g;
    geod_init(&g, e.equator_radius(), e.flattening());

    double dist;
    geod_inverse(&g, p1.first, p1.second, p2.first, p2.second, &dist, NULL, NULL);

    return dist;
}

double geodesic_distance(const S2Point &p,
                         const counted_t<const ql::datum_t> &g,
                         const ellipsoid_spec_t &e) {
    class distance_estimator_t : public s2_geo_visitor_t<double> {
    public:
        distance_estimator_t(
                lat_lon_point_t r, const S2Point &r_s2, const ellipsoid_spec_t &_e)
            : ref_(r), ref_s2_(r_s2), e_(_e) { }
        double on_point(const S2Point &point) {
            lat_lon_point_t llpoint =
                lat_lon_point_t(S2LatLng::Latitude(point).degrees(),
                                S2LatLng::Longitude(point).degrees());
            return geodesic_distance(ref_, llpoint, e_);
        }
        double on_line(const S2Polyline &line) {
            // This sometimes over-estimates large distances, because the
            // projection assumes spherical rather than ellipsoid geometry.
            int next_vertex;
            S2Point prj = line.Project(ref_s2_, &next_vertex);
            if (prj == ref_s2_) {
                // ref_ is on the line
                return 0.0;
            } else {
                lat_lon_point_t llprj =
                    lat_lon_point_t(S2LatLng::Latitude(prj).degrees(),
                                    S2LatLng::Longitude(prj).degrees());
                return geodesic_distance(ref_, llprj, e_);
            }
        }
        double on_polygon(const S2Polygon &polygon) {
            // This sometimes over-estimates large distances, because the
            // projection assumes spherical rather than ellipsoid geometry.
            S2Point prj = polygon.Project(ref_s2_);
            if (prj == ref_s2_) {
                // ref_ is inside/on the polygon
                return 0.0;
            } else {
                lat_lon_point_t llprj =
                    lat_lon_point_t(S2LatLng::Latitude(prj).degrees(),
                                    S2LatLng::Longitude(prj).degrees());
                return geodesic_distance(ref_, llprj, e_);
            }
        }
        lat_lon_point_t ref_;
        const S2Point &ref_s2_;
        const ellipsoid_spec_t &e_;
    };
    distance_estimator_t estimator(
        lat_lon_point_t(S2LatLng::Latitude(p).degrees(),
                        S2LatLng::Longitude(p).degrees()),
        p, e);
    return visit_geojson(&estimator, g);
}

lat_lon_point_t geodesic_point_at_dist(const lat_lon_point_t &p,
                                       double dist,
                                       double azimuth,
                                       const ellipsoid_spec_t &e) {
    // Use Karney's algorithm
    struct geod_geodesic g;
    geod_init(&g, e.equator_radius(), e.flattening());

    double lat, lon;
    geod_direct(&g, p.first, p.second, azimuth, dist, &lat, &lon, NULL);

    return lat_lon_point_t(lat, lon);
}

dist_unit_t parse_dist_unit(const std::string &s) {
    if (s == "m") {
        return dist_unit_t::M;
    } else if (s == "km") {
        return dist_unit_t::KM;
    } else if (s == "mi") {
        return dist_unit_t::MI;
    } else if (s == "nm") {
        return dist_unit_t::NM;
    } else if (s == "ft") {
        return dist_unit_t::FT;
    } else {
        throw geo_exception_t("Unrecognized distance unit: " + s);
    }
}

double unit_to_meters(dist_unit_t u) {
    switch (u) {
        case dist_unit_t::M: return 1.0;
        case dist_unit_t::KM: return 1000.0;
        case dist_unit_t::MI: return 1609.344;
        case dist_unit_t::NM: return 1852.0;
        case dist_unit_t::FT: return 0.3048;
        default: unreachable();
    }
}

double convert_dist_unit(double d, dist_unit_t from, dist_unit_t to) {
    // First convert to meters
    double conv_factor = unit_to_meters(from);

    // Then to the result unit
    conv_factor /= unit_to_meters(to);

    return d * conv_factor;
}
