// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef GEO_GEO_VISITOR_HPP_
#define GEO_GEO_VISITOR_HPP_

#include "rdb_protocol/geo/s2/util/math/vector3.h"

typedef Vector3_d S2Point;
class S2Polyline;
class S2Polygon;

template <class return_t>
class s2_geo_visitor_t {
public:
    virtual ~s2_geo_visitor_t() { }

    virtual return_t on_point(const S2Point &) = 0;
    virtual return_t on_line(const S2Polyline &) = 0;
    virtual return_t on_polygon(const S2Polygon &) = 0;
};

#endif  // GEO_GEO_VISITOR_HPP_
