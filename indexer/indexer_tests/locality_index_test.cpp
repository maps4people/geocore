#include "testing/testing.hpp"

#include "indexer/cell_id.hpp"
#include "indexer/covering_index.hpp"
#include "indexer/covering_index_builder.hpp"
#include "indexer/covered_object.hpp"

#include "coding/file_container.hpp"
#include "coding/reader.hpp"

#include "geometry/rect2d.hpp"

#include "base/geo_object_id.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

using namespace indexer;
using namespace std;

namespace
{
template <class ObjectsVector, class Writer>
void BuildGeoObjectsIndex(ObjectsVector const & objects, Writer && writer)
{
  base::thread_pool::computational::ThreadPool threadPool{1};
  indexer::GeoObjectsIndexBuilder indexBuilder{threadPool};

  covering::ObjectsCovering objectsCovering;
  for (auto const & object : objects)
    indexBuilder.Cover(object, objectsCovering);

  indexBuilder.BuildCoveringIndex(std::move(objectsCovering), std::forward<Writer>(writer),
                                  kGeoObjectsDepthLevels);
}

using Ids = set<uint64_t>;
using RankedIds = vector<uint64_t>;

template <typename CoveringIndex>
Ids GetIds(CoveringIndex const & index, m2::RectD const & rect)
{
  Ids ids;
  index.ForEachInRect([&ids](base::GeoObjectId const & id) { ids.insert(id.GetEncodedId()); },
                      rect);
  return ids;
};

template <typename CoveringIndex>
RankedIds GetRankedIds(CoveringIndex const & index, m2::PointD const & center,
                       m2::PointD const & border, uint32_t topSize)
{
  RankedIds ids;
  index.ForClosestToPoint(
      [&ids](base::GeoObjectId const & id, auto) { ids.push_back(id.GetEncodedId()); }, center,
      MercatorBounds::DistanceOnEarth(center, border), topSize);
  return ids;
};

UNIT_TEST(BuildCoveringIndexTest)
{
  vector<CoveredObject> objects;
  objects.resize(4);
  objects[0].SetForTesting(1, m2::PointD{0, 0});
  objects[1].SetForTesting(2, m2::PointD{1, 0});
  objects[2].SetForTesting(3, m2::PointD{1, 1});
  objects[3].SetForTesting(4, m2::PointD{0, 1});

  vector<uint8_t> localityIndex;
  MemWriter<vector<uint8_t>> writer(localityIndex);
  BuildGeoObjectsIndex(objects, writer);
  MemReader reader(localityIndex.data(), localityIndex.size());

  indexer::GeoObjectsIndex<MemReader> index(reader);

  TEST_EQUAL(GetIds(index, m2::RectD{-0.5, -0.5, 0.5, 0.5}), (Ids{1}), ());
  TEST_EQUAL(GetIds(index, m2::RectD{0.5, -0.5, 1.5, 1.5}), (Ids{2, 3}), ());
  TEST_EQUAL(GetIds(index, m2::RectD{-0.5, -0.5, 1.5, 1.5}), (Ids{1, 2, 3, 4}), ());
}

UNIT_TEST(CoveringIndexRankTest)
{
  vector<CoveredObject> objects;
  objects.resize(4);
  objects[0].SetForTesting(1, m2::PointD{1, 0});
  objects[1].SetForTesting(2, m2::PointD{2, 0});
  objects[2].SetForTesting(3, m2::PointD{3, 0});
  objects[3].SetForTesting(4, m2::PointD{4, 0});

  vector<uint8_t> localityIndex;
  MemWriter<vector<uint8_t>> writer(localityIndex);
  BuildGeoObjectsIndex(objects, writer);
  MemReader reader(localityIndex.data(), localityIndex.size());

  indexer::GeoObjectsIndex<MemReader> index(reader);
  TEST_EQUAL(GetRankedIds(index, m2::PointD{1, 0} /* center */, m2::PointD{4, 0} /* border */,
                          4 /* topSize */),
             (vector<uint64_t>{1, 2, 3, 4}), ());
  TEST_EQUAL(GetRankedIds(index, m2::PointD{1, 0} /* center */, m2::PointD{3, 0} /* border */,
                          4 /* topSize */),
             (vector<uint64_t>{1, 2, 3}), ());
  TEST_EQUAL(GetRankedIds(index, m2::PointD{4, 0} /* center */, m2::PointD{1, 0} /* border */,
                          4 /* topSize */),
             (vector<uint64_t>{4, 3, 2, 1}), ());
  TEST_EQUAL(GetRankedIds(index, m2::PointD{4, 0} /* center */, m2::PointD{1, 0} /* border */,
                          2 /* topSize */),
             (vector<uint64_t>{4, 3}), ());
  TEST_EQUAL(GetRankedIds(index, m2::PointD{3, 0} /* center */, m2::PointD{0, 0} /* border */,
                          1 /* topSize */),
             (vector<uint64_t>{3}), ());
}

UNIT_TEST(CoveringIndexTopSizeTest)
{
  vector<CoveredObject> objects;
  objects.resize(8);
  // Same cell.
  objects[0].SetForTesting(1, m2::PointD{1.0, 0.0});
  objects[1].SetForTesting(2, m2::PointD{1.0, 0.0});
  objects[2].SetForTesting(3, m2::PointD{1.0, 0.0});
  objects[3].SetForTesting(4, m2::PointD{1.0, 0.0});
  // Another close cell.
  objects[4].SetForTesting(5, m2::PointD{1.0, 1.0});
  objects[5].SetForTesting(6, m2::PointD{1.0, 1.0});
  // Far cell.
  objects[6].SetForTesting(7, m2::PointD{10.0, 10.0});
  // The big object contains all points and must be returned on any query.
  objects[7].SetForTesting(8, m2::RectD{0.0, 0.0, 10.0, 10.0});

  vector<uint8_t> localityIndex;
  MemWriter<vector<uint8_t>> writer(localityIndex);
  BuildGeoObjectsIndex(objects, writer);
  MemReader reader(localityIndex.data(), localityIndex.size());

  indexer::GeoObjectsIndex<MemReader> index(reader);

  // There is only one object (the big object) at this point.
  TEST_EQUAL(GetRankedIds(index, m2::PointD{2.0, 2.0} /* center */,
                          m2::PointD{2.0, 2.0} /* border */, 8 /* topSize */)
                 .size(),
             1, ());

  // There are 4 small objects and 1 big object at this point.
  TEST_EQUAL(GetRankedIds(index, m2::PointD{1.0, 0.0} /* center */,
                          m2::PointD{10.0, 10.0} /* border */, 5 /* topSize */)
                 .size(),
             5, ());

  // 4 objects are indexed at the central cell. Index does not guarantee the order but must
  // return 4 objects from central cell and the big object.
  TEST_EQUAL(GetRankedIds(index, m2::PointD{1.0, 0.0} /* center */,
                          m2::PointD{10.0, 10.0} /* border */, 3 /* topSize */)
                 .size(),
             5, ());

  // At the {1.0, 1.0} point there are also 2 objects, but it's not a central cell, index must
  // return 5 (topSize) objects.
  TEST_EQUAL(GetRankedIds(index, m2::PointD{1.0, 0.0} /* center */,
                          m2::PointD{10.0, 10.0} /* border */, 5 /* topSize */)
                 .size(),
             5, ());

  // The same here. There are not too many objects in central cell. Index must return 5 (topSize)
  // objects.
  TEST_EQUAL(GetRankedIds(index, m2::PointD{4.0, 0.0} /* center */,
                          m2::PointD{10.0, 10.0} /* border */, 5 /* topSize */)
                 .size(),
             5, ());

  TEST_EQUAL(GetRankedIds(index, m2::PointD{4.0, 0.0} /* center */,
                          m2::PointD{10.0, 10.0} /* border */, 8 /* topSize */)
                 .size(),
             8, ());
}

UNIT_TEST(CoveringIndexWeightRankTest)
{
  m2::PointD queryPoint{0, 0};
  m2::PointD queryBorder{0, 2};

  vector<CoveredObject> objects;
  objects.resize(7);
  // Enclose query point.
  objects[0].SetForTesting(1, m2::PointD{0, 0});
  objects[1].SetForTesting(2, m2::PointD{0.000001, 0.000001}); // in the same lowermost cell
  objects[2].SetForTesting(3, m2::RectD{-1, -1, 1, 1});
  // Closest objects.
  objects[3].SetForTesting(4, m2::RectD{0.5, 0.5, 1.0, 1.0});
  objects[4].SetForTesting(5, m2::PointD{1, 0});
  objects[5].SetForTesting(6, m2::PointD{1, 1});
  objects[6].SetForTesting(7, m2::RectD{1, 0, 1.1, 0.1});

  vector<uint8_t> localityIndex;
  MemWriter<vector<uint8_t>> writer(localityIndex);
  BuildGeoObjectsIndex(objects, writer);
  MemReader reader(localityIndex.data(), localityIndex.size());

  indexer::GeoObjectsIndex<MemReader> index(reader);

  vector<pair<uint64_t, double>> ids;
  index.ForClosestToPoint(
      [&ids](base::GeoObjectId const & id, auto weight) { ids.push_back({id.GetEncodedId(), weight}); },
      queryPoint, MercatorBounds::DistanceOnEarth(queryPoint, queryBorder),
      7 /* topSize */);

  TEST_EQUAL(ids.size(), 7, ());

  // Enclose objects: "1", "2", "3".
  TEST_EQUAL((map<uint64_t, double>(ids.begin(), ids.begin() + 3)),
             (map<uint64_t, double>{{1, 1.0}, {2, 1.0}, {3, 1.0}}), ());
  // "4"
  TEST_EQUAL(ids[3].first, 4, ());
  TEST_LESS(ids[3].second, 1.0, ());
  // "5", "6", "7"
  TEST_EQUAL((set<uint64_t>{ids[4].first, ids[5].first, ids[6].first}), (set<uint64_t>{5, 6, 7}), ());
  TEST(ids[4].second < ids[3].second, ());
  TEST(ids[5].second < ids[3].second, ());
  TEST(ids[6].second < ids[3].second, ());
}

}  // namespace
