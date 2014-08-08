// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef GEO_INCLUSION_HPP_
#define GEO_INCLUSION_HPP_

#include "containers/counted.hpp"
#include "rdb_protocol/geo/s2/util/math/vector3.h"

typedef Vector3_d S2Point;
class S2Polyline;
class S2Polygon;
namespace ql {
class datum_t;
}

/* A variant that works on a GeoJSON object */
bool geo_does_include(const S2Polygon &polygon,
                      const counted_t<const ql::datum_t> &g);

/* Variants for S2 geometry */
bool geo_does_include(const S2Polygon &polygon,
                      const S2Point &g);
bool geo_does_include(const S2Polygon &polygon,
                      const S2Polyline &g);
bool geo_does_include(const S2Polygon &polygon,
                      const S2Polygon &g);

#endif  // GEO_INCLUSION_HPP_
