#pragma once

#include "indexer/feature_data.hpp"

#include "coding/file_reader.hpp"
#include "coding/file_writer.hpp"
#include "coding/read_write_utils.hpp"

#include "base/geo_object_id.hpp"
#include "base/stl_helpers.hpp"
#include "base/thread_pool_delayed.hpp"

#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/iostreams/device/mapped_file.hpp>

namespace serial
{
class GeometryCodingParams;
}  // namespace serial

namespace feature
{
class FeatureBuilder
{
public:
  using PointSeq = std::vector<m2::PointD>;
  using Geometry = std::list<PointSeq>;
  using Buffer = std::vector<char>;
  using Offsets = std::vector<uint32_t>;

  struct SupportingData
  {
    Offsets m_ptsOffset;
    Offsets m_trgOffset;
    uint8_t m_ptsMask = 0;
    uint8_t m_trgMask = 0;
    uint32_t m_ptsSimpMask = 0;
    PointSeq m_innerPts;
    PointSeq m_innerTrg;
    Buffer m_buffer;
  };

  FeatureBuilder();
  // Checks for equality. The error of coordinates is allowed.
  bool operator==(FeatureBuilder const & fb) const;
  // Checks for equality. The error of coordinates isn't allowed. Binary equality check of
  // coordinates is used.
  bool IsExactEq(FeatureBuilder const & fb) const;

  // To work with geometry.
  void AddPoint(m2::PointD const & p);
  void SetHoles(Geometry const & holes);
  void AddPolygon(std::vector<m2::PointD> & poly);
  void ResetGeometry();
  m2::RectD const & GetLimitRect() const { return m_limitRect; }
  Geometry const & GetGeometry() const { return m_polygons; }
  PointSeq const & GetOuterGeometry() const { return m_polygons.front(); }
  GeomType GetGeomType() const { return m_params.GetGeomType(); }
  bool IsGeometryClosed() const;
  m2::PointD GetGeometryCenter() const;
  m2::PointD GetKeyPoint() const;
  size_t GetPointsCount() const;
  size_t GetPolygonsCount() const { return m_polygons.size(); }
  size_t GetTypesCount() const { return m_params.m_types.size(); }

  template <class ToDo>
  void ForEachGeometryPointEx(ToDo && toDo) const
  {
    if (IsPoint())
    {
      toDo(m_center);
    }
    else
    {
      for (PointSeq const & points : m_polygons)
      {
        for (auto const & pt : points)
        {
          if (!toDo(pt))
            return;
        }
        toDo.EndRegion();
      }
    }
  }

  template <class ToDo>
  void ForEachGeometryPoint(ToDo && toDo) const
  {
    ToDoWrapper<ToDo> wrapper(std::forward<ToDo>(toDo));
    ForEachGeometryPointEx(std::move(wrapper));
  }

  template <class ToDo>
  bool ForAnyGeometryPointEx(ToDo && toDo) const
  {
    if (IsPoint())
      return toDo(m_center);

    for (PointSeq const & points : m_polygons)
    {
      for (auto const & pt : points)
      {
        if (toDo(pt))
          return true;
      }
      toDo.EndRegion();
    }
    return false;
  }

  template <class ToDo>
  bool ForAnyGeometryPoint(ToDo && toDo) const
  {
    ToDoWrapper<ToDo> wrapper(std::forward<ToDo>(toDo));
    return ForAnyGeometryPointEx(std::move(wrapper));
  }

  // To work with geometry type.
  void SetCenter(m2::PointD const & p);
  void SetLinear(bool reverseGeometry = false);
  void SetArea() { m_params.SetGeomType(GeomType::Area); }
  bool IsPoint() const { return GetGeomType() == GeomType::Point; }
  bool IsLine() const { return GetGeomType() == GeomType::Line; }
  bool IsArea() const { return GetGeomType() == GeomType::Area; }

  // To work with types.
  void SetType(uint32_t type) { m_params.SetType(type); }
  void AddType(uint32_t type) { m_params.AddType(type); }
  bool PopExactType(uint32_t type) { return m_params.PopExactType(type); }

  template <class Fn>
  bool RemoveTypesIf(Fn && fn)
  {
    base::EraseIf(m_params.m_types, std::forward<Fn>(fn));
    return m_params.m_types.empty();
  }

  bool HasType(uint32_t t) const { return m_params.IsTypeExist(t); }
  bool HasType(uint32_t t, uint8_t level) const { return m_params.IsTypeExist(t, level); }
  uint32_t FindType(uint32_t comp, uint8_t level) const { return m_params.FindType(comp, level); }
  FeatureParams::Types const & GetTypes() const { return m_params.m_types; }

  // To work with additional information.
  void SetRank(uint8_t rank);
  void AddHouseNumber(std::string const & houseNumber);
  void AddStreet(std::string const & streetName);
  void AddPostcode(std::string const & postcode);
  bool AddName(std::string const & lang, std::string const & name);
  void SetParams(FeatureParams const & params) { m_params.SetParams(params); }

  FeatureParams const & GetParams() const { return m_params; }
  FeatureParams & GetParams() { return m_params; }
  std::string GetName(int8_t lang = StringUtf8Multilang::kDefaultCode) const;
  StringUtf8Multilang const & GetMultilangName() const { return m_params.name; }
  uint8_t GetRank() const { return m_params.rank; }
  bool FormatFullAddress(std::string & res) const;
  AddressData const & GetAddressData() const { return m_params.GetAddressData(); }

  Metadata const & GetMetadata() const { return m_params.GetMetadata(); }
  Metadata & GetMetadata() { return m_params.GetMetadata(); }

  // To work with types and names based on drawing.
  // Check classificator types for their compatibility with feature geometry type.
  // Need to call when using any classificator types manipulating.
  // Return false If no any valid types.
  bool RemoveInvalidTypes();
  // Clear name if it's not visible in scale range [minS, maxS].
  void RemoveNameIfInvisible(int minS = 0, int maxS = 1000);
  void RemoveUselessNames();
  int GetMinFeatureDrawScale() const;
  bool IsDrawableInRange(int lowScale, int highScale) const;

  // Serialization.
  bool PreSerialize();
  void SerializeBase(Buffer & data, serial::GeometryCodingParams const & params,
                     bool saveAddInfo) const;

  bool PreSerializeAndRemoveUselessNamesForIntermediate();
  void SerializeForIntermediate(Buffer & data) const;
  void SerializeBorderForIntermediate(serial::GeometryCodingParams const & params,
                                      Buffer & data) const;
  void DeserializeFromIntermediate(Buffer & data);

  // These methods use geometry without loss of accuracy.
  void SerializeAccuratelyForIntermediate(Buffer & data) const;
  void DeserializeAccuratelyFromIntermediate(Buffer & data);

  bool PreSerializeAndRemoveUselessNamesForMwm(SupportingData const & data);
  void SerializeCoveredObject(serial::GeometryCodingParams const & params,
                              SupportingData & data) const;
  void SerializeForMwm(SupportingData & data, serial::GeometryCodingParams const & params) const;

  // Get common parameters of feature.
  TypesHolder GetTypesHolder() const;

  // To work with osm ids.
  void AddOsmId(base::GeoObjectId id);
  void SetOsmId(base::GeoObjectId id);
  base::GeoObjectId GetFirstOsmId() const;
  base::GeoObjectId GetLastOsmId() const;
  // Returns an id of the most general element: node's one if there is no area or relation,
  // area's one if there is no relation, and relation id otherwise.
  base::GeoObjectId GetMostGenericOsmId() const;
  bool HasOsmId(base::GeoObjectId const & id) const;
  bool HasOsmIds() const { return !m_osmIds.empty(); }
  std::vector<base::GeoObjectId> const & GetOsmIds() const { return m_osmIds; }

  // To work with coasts.
  void SetCoastCell(int64_t iCell) { m_coastCell = iCell; }
  bool IsCoastCell() const { return (m_coastCell != -1); }

protected:
  template <class ToDo>
  class ToDoWrapper
  {
  public:
    ToDoWrapper(ToDo && toDo) : m_toDo(std::forward<ToDo>(toDo)) {}
    bool operator()(m2::PointD const & p) { return m_toDo(p); }
    void EndRegion() {}

  private:
    ToDo && m_toDo;
  };

  // Can be one of the following:
  // - point in point-feature
  // - origin point of text [future] in line-feature
  // - origin point of text or symbol in area-feature
  m2::PointD m_center;  // Check  HEADER_HAS_POINT
  // List of geometry polygons.
  Geometry m_polygons;  // Check HEADER_IS_AREA
  m2::RectD m_limitRect;
  std::vector<base::GeoObjectId> m_osmIds;
  FeatureParams m_params;
  /// Not used in GEOM_POINTs
  int64_t m_coastCell;
};

void Check(FeatureBuilder const fb);
std::string DebugPrint(FeatureBuilder const & fb);

// SerializationPolicy serialization and deserialization.
namespace serialization_policy
{
enum class SerializationVersion : uint32_t
{
  Undefined,
  MinSize,
  MaxAccuracy
};

using TypeSerializationVersion = typename std::underlying_type<SerializationVersion>::type;

struct MinSize
{
  auto static const kSerializationVersion =
      static_cast<TypeSerializationVersion>(SerializationVersion::MinSize);

  static void Serialize(FeatureBuilder const & fb, FeatureBuilder::Buffer & data)
  {
    fb.SerializeForIntermediate(data);
  }

  static void Deserialize(FeatureBuilder & fb, FeatureBuilder::Buffer & data)
  {
    fb.DeserializeFromIntermediate(data);
  }
};

struct MaxAccuracy
{
  auto static const kSerializationVersion =
      static_cast<TypeSerializationVersion>(SerializationVersion::MinSize);

  static void Serialize(FeatureBuilder const & fb, FeatureBuilder::Buffer & data)
  {
    fb.SerializeAccuratelyForIntermediate(data);
  }

  static void Deserialize(FeatureBuilder & fb, FeatureBuilder::Buffer & data)
  {
    fb.DeserializeAccuratelyFromIntermediate(data);
  }
};
}  // namespace serialization_policy

// Features file processing ------------------------------------------------------------------------
// Read feature from feature source.
template <class SerializationPolicy = serialization_policy::MinSize, class Source>
void ReadFromSourceRawFormat(Source & src, FeatureBuilder & fb)
{
  uint32_t const sz = ReadVarUint<uint32_t>(src);
  typename FeatureBuilder::Buffer buffer(sz);
  src.Read(&buffer[0], sz);
  SerializationPolicy::Deserialize(fb, buffer);
}

class FeaturesFileMmap
{
public:
  class const_iterator;

  FeaturesFileMmap(std::string const & filename);

  template <typename SerializationPolicy, typename Handler>
  void ForEachTaskChunk(unsigned int taskIndex, unsigned int tasksCount, size_t chunkSize,
                        Handler && handler) const
  {
    auto && reader =
        MemReaderTemplate<true /* WithExceptions */>{m_fileMmap.data(), m_fileMmap.size()};
    auto && src = ReaderSource<MemReaderTemplate<true>>{reader};

    auto && buffer = FeatureBuilder::Buffer{};
    auto && fileSize = reader.Size();

    for (size_t featuresCounter = 0; src.Pos() < fileSize; ++featuresCounter)
    {
      auto const featurePos = src.Pos();

      uint32_t const featureSize = ReadVarUint<uint32_t>(src);

      auto const featureChunkIndex = featuresCounter / chunkSize;
      auto const featureTaskIndex = featureChunkIndex % tasksCount;
      if (featureTaskIndex != taskIndex)
      {
        src.Skip(featureSize);
        continue;
      }

      buffer.resize(featureSize);
      src.Read(buffer.data(), featureSize);

      auto && fb = FeatureBuilder{};
      SerializationPolicy::Deserialize(fb, buffer);

      handler(fb, featurePos);
    }
  }

private:
  boost::iostreams::mapped_file_source m_fileMmap;
};

// Process features in .dat file.
template <class SerializationPolicy = serialization_policy::MinSize, class Handler>
void ForEachFromDatRawFormat(std::string const & filename, Handler && handler)
{
  // It is not possible to map a file of zero size.
  if (!boost::filesystem::file_size(filename))
    return;
  auto && featuresMmap = FeaturesFileMmap{filename};

  featuresMmap.ForEachTaskChunk<SerializationPolicy>(
      0 /* taskIndex */, 1 /* taskCount*/, 1 /* chunkSize */, std::forward<Handler>(handler));
}

// Parallel process features in .dat file.
template <class SerializationPolicy = serialization_policy::MinSize, class ProcessorMaker>
void ProcessParallelFromDatRawFormat(unsigned int threadsCount, uint64_t chunkSize,
                                     std::string const & filename,
                                     ProcessorMaker && processorMaker)
{
  CHECK_GREATER_OR_EQUAL(threadsCount, 1, ());
  if (threadsCount == 0 || threadsCount == 1)
    return ForEachFromDatRawFormat<SerializationPolicy>(filename, processorMaker());

  // It is not possible to map a file of zero size.
  if (!boost::filesystem::file_size(filename))
    return;
  auto && featuresMmap = FeaturesFileMmap{filename};

  auto && threads = std::vector<std::thread>{};
  for (unsigned int i = 0; i < threadsCount; ++i)
  {
    auto && processor = processorMaker();
    threads.emplace_back([i, threadsCount, chunkSize, &featuresMmap,
                          processor = std::move(processor)]() mutable {
      featuresMmap.ForEachTaskChunk<SerializationPolicy>(i, threadsCount, chunkSize, processor);
    });
  }

  for (auto & thread : threads)
    thread.join();
}

// Parallel process features in .dat file by 1'000 items in chunk.
template <class SerializationPolicy = serialization_policy::MinSize, class ProcessorMaker>
void ProcessParallelFromDatRawFormat(unsigned int threadsCount, std::string const & filename,
                                     ProcessorMaker && processorMaker)
{
  ProcessParallelFromDatRawFormat<SerializationPolicy>(
      threadsCount, 1'000 /* chunkSize */, filename, std::forward<ProcessorMaker>(processorMaker));
}

// Parallel process features in .dat file.
template <class SerializationPolicy = serialization_policy::MinSize, class Handler>
void ForEachParallelFromDatRawFormat(unsigned int threadsCount, std::string const & filename,
                                     Handler && handler)
{
  ProcessParallelFromDatRawFormat<SerializationPolicy>(
      threadsCount, filename, [&handler] { return std::forward<Handler>(handler); });
}

template <class SerializationPolicy = serialization_policy::MinSize>
std::vector<FeatureBuilder> ReadAllDatRawFormat(std::string const & fileName)
{
  std::vector<FeatureBuilder> fbs;
  ForEachFromDatRawFormat<SerializationPolicy>(fileName, [&](auto && fb, auto const &) {
    fbs.emplace_back(std::move(fb));
  });
  return fbs;
}

template <class SerializationPolicy = serialization_policy::MinSize, class Writer = FileWriter>
class FeatureBuilderWriter
{
public:
  explicit FeatureBuilderWriter(std::string const & filename,
                                FileWriter::Op op = FileWriter::Op::OP_WRITE_TRUNCATE)
    : m_writer(filename, op)
  {
    // TODO(maksimandrianov): I would like to support the verification of serialization versions,
    // but this requires reworking of FeatureCollector class and its derived classes. It is in
    // future plans WriteVarUint(m_writer,
    // static_cast<serialization_policy::TypeSerializationVersion>(SerializationPolicy::kSerializationVersion));
  }

  void Write(FeatureBuilder const & fb)
  {
    FeatureBuilder::Buffer buffer;
    SerializationPolicy::Serialize(fb, buffer);
    WriteVarUint(m_writer, static_cast<uint32_t>(buffer.size()));
    m_writer.Write(buffer.data(), buffer.size() * sizeof(FeatureBuilder::Buffer::value_type));
  }

private:
  Writer m_writer;
};
}  // namespace feature
