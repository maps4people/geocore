#include "generator/covering_index_generator.hpp"
#include "generator/data_version.hpp"
#include "generator/generate_info.hpp"
#include "generator/geo_objects/geo_objects_generator.hpp"
#include "generator/osm_source.hpp"
#include "generator/processor_factory.hpp"
#include "generator/raw_generator.hpp"
#include "generator/regions/collector_region_info.hpp"
#include "generator/regions/regions.hpp"
#include "generator/statistics.hpp"
#include "generator/streets/streets.hpp"
#include "generator/translator_collection.hpp"
#include "generator/translator_factory.hpp"

#include "indexer/classificator_loader.hpp"
#include "indexer/covering_index_builder.hpp"
#include "indexer/features_vector.hpp"
#include "indexer/map_style_reader.hpp"

#include "platform/platform.hpp"

#include "coding/endianness.hpp"

#include "base/file_name_utils.hpp"

#include <boost/optional.hpp>
#include <boost/program_options.hpp>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>

#define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
#include <boost/stacktrace.hpp>

#include "defines.hpp"

using namespace std;

namespace
{
char const * GetDataPathHelp()
{
  static string const kHelp =
      "Directory where the generated mwms are put into. Also used as the path for helper "
      "functions, such as those that calculate statistics and regenerate sections. ";

  return kHelp.c_str();
}
}  // namespace

struct CliCommandOptions
{
  std::string m_osm_file_type;
  std::string m_osm_file_name;
  std::string m_node_storage;
  std::string m_user_resource_path;
  std::string m_data_path;
  std::string m_nodes_list_path;
  std::string m_regions_features;
  std::string m_ids_without_addresses;
  std::string m_geo_objects_key_value;
  std::string m_regions_index;
  std::string m_regions_key_value;
  std::string m_streets_key_value;
  std::string m_streets_features;
  std::string m_geo_objects_features;
  std::string m_geo_objects_index;
  std::string m_key_value;
  bool m_preprocess = false;
  bool m_generate_region_features = false;
  bool m_generate_features = false;
  bool m_generate_regions = false;
  bool m_generate_geo_objects_index = false;
  bool m_generate_regions_kv = false;
  bool m_generate_streets_features = false;
  bool m_generate_geo_objects_features = false;
  bool m_verbose = false;
};

namespace po = boost::program_options;

CliCommandOptions DefineOptions(int argc, char * argv[])
{
  CliCommandOptions o;
  po::options_description optionsDescription;

  optionsDescription.add_options()
     ("osm_file_name",
         po::value(&o.m_osm_file_name)->default_value(""),
         "Input osm area file.")
     ("osm_file_type",
         po::value(&o.m_osm_file_type)->default_value("xml"),
         "Input osm area file type [xml, o5m].")
     ("data_path",
         po::value(&o.m_data_path)->default_value(""),
         GetDataPathHelp())
     ("user_resource_path",
         po::value(&o.m_user_resource_path)->default_value(""),
         "User defined resource path for classificator.txt and etc.")
     ("node_storage",
         po::value(&o.m_node_storage)->default_value("map"),
         "Type of storage for intermediate points representation. Available: raw, map, mem.")
     ("preprocess",
         po::value(&o.m_preprocess)->default_value(false),
         "1st pass - create nodes/ways/relations data.")
     ("generate_features",
         po::value(&o.m_generate_features)->default_value(false),
         "2nd pass - generate intermediate features.")
     ("generate_region_features",
         po::value(&o.m_generate_region_features)->default_value(false),
         "Generate intermediate features for regions to use in regions index and borders generation.")
     ("generate_streets_features",
         po::value(&o.m_generate_streets_features)->default_value(false),
         "Generate intermediate features for streets to use in server-side forward geocoder.")
     ("generate_geo_objects_features",
         po::value(&o.m_generate_geo_objects_features)->default_value(false),
         "Generate intermediate features for geo objects to use in geo objects index.")
     ("generate_geo_objects_index",
         po::value(&o.m_generate_geo_objects_index)->default_value(false),
         "Generate objects and index for server-side reverse geocoder.")
     ("generate_regions",
         po::value(&o.m_generate_regions)->default_value(false),
         "Generate regions index and borders for server-side reverse geocoder.")
     ("generate_regions_kv",
         po::value(&o.m_generate_regions_kv)->default_value(false),
         "Generate regions key-value for server-side reverse geocoder.")
     ("nodes_list_path",
         po::value(&o.m_nodes_list_path)->default_value(""),
         "Path to file containing list of node ids we need to add to locality index. May be empty.")
     ("regions_index",
         po::value(&o.m_regions_index)->default_value(""),
         "Input/Output regions index file.")
     ("regions_key_value",
         po::value(&o.m_regions_key_value)->default_value(""),
         "Input/Output regions key-value file.")
     ("streets_features",
         po::value(&o.m_streets_features)->default_value(""),
         "Input/Output tmp.mwm file with streets.")
     ("streets_key_value",
         po::value(&o.m_streets_key_value)->default_value(""),
         "Output streets key-value file.")
     ("geo_objects_features",
         po::value(&o.m_geo_objects_features)->default_value(""),
         "Input/Output tmp.mwm file with geo objects.")
     ("ids_without_addresses",
         po::value(&o.m_ids_without_addresses)->default_value(""),
         "Output file with objects ids without addresses.")
     ("geo_objects_index",
         po::value(&o.m_geo_objects_index)->default_value(""),
         "Input/Output geo objects index file.")
     ("geo_objects_key_value",
         po::value(&o.m_geo_objects_key_value)->default_value(""),
         "Input/Output geo objects key-value file.")
     ("regions_features",
         po::value(&o.m_regions_features)->default_value(""),
         "Input/Output tmp.mwm file with regions.")
     ("key_value",
         po::value(&o.m_key_value)->default_value(""),
         "Input key-value file (.jsonl or .jsonl.gz).")
     ("verbose",
         po::value(&o.m_verbose)->default_value(false),
         "Provide more detailed output.")
     ("version", "get version")
     ("help", "produce help message");

  po::variables_map vm;

  po::store(po::parse_command_line(argc, argv, optionsDescription), vm);
  po::notify(vm);

  if (vm.count("help"))
  {
    std::cout << optionsDescription << std::endl;
    exit(0);
  }

  if (vm.count("version"))
  {
    std::cout << generator::DataVersion::GetCodeVersion() << std::endl;
    exit(0);
  }

  return o;
}

using namespace generator;

int GeneratorToolMain(int argc, char ** argv)
{
  CHECK(IsLittleEndian(), ("Only little-endian architectures are supported."));

  CliCommandOptions options;

  options = DefineOptions(argc, argv);

  Platform & pl = GetPlatform();

  if (options.m_user_resource_path.empty())
  {
    LOG(LERROR, ("Set user resource path"));
    return EXIT_FAILURE;
  }

  if (options.m_data_path.empty())
  {
    LOG(LERROR, ("Set data path path"));
    return EXIT_FAILURE;
  }

  pl.SetWritableDir(options.m_data_path);
  pl.SetResourceDir(options.m_user_resource_path);

  string const path = base::AddSlashIfNeeded(options.m_data_path);

  feature::GenerateInfo genInfo;
  genInfo.m_threadsCount = pl.CpuCores();
  genInfo.m_verbose = options.m_verbose;
  genInfo.m_dataPath = path;
  genInfo.m_targetDir = path;
  genInfo.m_tmpDir = path;

  /// @todo Probably, it's better to add separate option for .mwm.tmp files.
  if (!options.m_data_path.empty())
  {
    string const tmpPath = base::JoinPath(genInfo.m_dataPath, "tmp");
    if (Platform::MkDir(tmpPath) != Platform::ERR_UNKNOWN)
      genInfo.m_tmpDir = tmpPath;
  }
  if (!options.m_node_storage.empty())
    genInfo.SetNodeStorageType(options.m_node_storage);
  if (!options.m_osm_file_type.empty())
    genInfo.SetOsmFileType(options.m_osm_file_type);

  genInfo.m_osmFileName = options.m_osm_file_name;

  auto const regionsInfoPath =
      genInfo.GetTmpFileName("region", regions::CollectorRegionInfo::kDefaultExt);

  // Use merged style.
  GetStyleReader().SetCurrentStyle(MapStyleMerged);

  classificator::Load();

  // Generate intermediate files.
  if (options.m_preprocess)
  {
    DataVersion{options.m_osm_file_name}.DumpToPath(genInfo.m_dataPath);

    LOG(LINFO, ("Generating intermediate data ...."));
    if (!GenerateIntermediateData(genInfo))
      return EXIT_FAILURE;
  }

  // Generate .mwm.tmp files.
  if (options.m_generate_features || options.m_generate_region_features ||
      options.m_generate_streets_features || options.m_generate_geo_objects_features)
  {
    RawGenerator rawGenerator(genInfo);
    if (options.m_generate_region_features)
      rawGenerator.GenerateRegionFeatures(options.m_regions_features, regionsInfoPath);
    if (options.m_generate_streets_features)
      rawGenerator.GenerateStreetsFeatures(options.m_streets_features);
    if (options.m_generate_geo_objects_features)
      rawGenerator.GenerateGeoObjectsFeatures(options.m_geo_objects_features);

    if (!rawGenerator.Execute())
      return EXIT_FAILURE;
  }

  if (!options.m_streets_key_value.empty())
  {
    streets::GenerateStreets(options.m_regions_index, options.m_regions_key_value,
                             options.m_streets_features, options.m_geo_objects_features,
                             options.m_streets_key_value, options.m_verbose,
                             genInfo.m_threadsCount);
  }

  if (!options.m_geo_objects_key_value.empty())
  {
    if (!geo_objects::GenerateGeoObjects(
            options.m_regions_index, options.m_regions_key_value, options.m_geo_objects_features,
            options.m_ids_without_addresses, options.m_geo_objects_key_value, options.m_verbose,
            genInfo.m_threadsCount))
      return EXIT_FAILURE;
  }

  if (options.m_generate_geo_objects_index)
  {
    if (options.m_geo_objects_index.empty())
    {
      LOG(LCRITICAL, ("Unspecified geo_objects_index file."));
      return EXIT_FAILURE;
    }

    auto const nodesListPath =
        boost::make_optional(!options.m_nodes_list_path.empty(), options.m_nodes_list_path);
    auto const streetsFeaturesPath =
        boost::make_optional(!options.m_streets_features.empty(), options.m_streets_features);

    LOG(LINFO, ("Saving geo objects index to", options.m_geo_objects_index));
    if (!GenerateGeoObjectsIndex(options.m_geo_objects_index, options.m_geo_objects_features,
                                 genInfo.m_threadsCount, nodesListPath, streetsFeaturesPath))
    {
      LOG(LCRITICAL, ("Error generating geo objects index."));
      return EXIT_FAILURE;
    }

    WriteDataVersionSection(options.m_geo_objects_index,
                            DataVersion::LoadFromPath(genInfo.m_dataPath).GetVersionJson());
  }

  if (options.m_generate_regions)
  {
    if (options.m_regions_index.empty())
    {
      LOG(LCRITICAL, ("Unspecified regions_index file."));
      return EXIT_FAILURE;
    }

    LOG(LINFO, ("Saving regions index to", options.m_regions_index));
    if (!GenerateRegionsIndex(options.m_regions_index, options.m_regions_features,
                              genInfo.m_threadsCount))
    {
      LOG(LCRITICAL, ("Error generating regions index."));
      return EXIT_FAILURE;
    }

    LOG(LINFO, ("Saving regions borders to", options.m_regions_index));
    if (!GenerateBorders(options.m_regions_index, options.m_regions_features))
    {
      LOG(LCRITICAL, ("Error generating regions borders."));
      return EXIT_FAILURE;
    }

    WriteDataVersionSection(options.m_regions_index,
                            DataVersion::LoadFromPath(genInfo.m_dataPath).GetVersionJson());
  }

  if (options.m_generate_regions_kv)
  {
    regions::GenerateRegions(options.m_regions_features, regionsInfoPath,
                             options.m_regions_key_value, options.m_verbose,
                             genInfo.m_threadsCount);
  }

  return 0;
}

void ErrorHandler(int signum)
{
  // Avoid recursive calls.
  signal(signum, SIG_DFL);

  // If there was an exception, then we will print the message.
  try
  {
    if (auto const eptr = current_exception())
      rethrow_exception(eptr);
  }
  catch (RootException const & e)
  {
    cerr << "Core exception: " << e.Msg() << "\n";
  }
  catch (exception const & e)
  {
    cerr << "Std exception: " << e.what() << "\n";
  }
  catch (...)
  {
    cerr << "Unknown exception.\n";
  }

  // Print stack stack.
  cerr << boost::stacktrace::stacktrace();
  // We raise the signal SIGABRT, so that there would be an opportunity to make a core dump.
  raise(SIGABRT);
}

int main(int argc, char ** argv)
{
  signal(SIGABRT, ErrorHandler);
  signal(SIGSEGV, ErrorHandler);
  try
  {
    return GeneratorToolMain(argc, argv);
  }
  catch (po::error & e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    return 1;
  }
  catch (std::exception & e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    return 1;
  }
}
