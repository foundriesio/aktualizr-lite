
#include <sys/file.h>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>

#include <boost/algorithm/string/join.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/program_options.hpp>

#include "aktualizr-lite/api.h"
#include "aktualizr-lite/cli/cli.h"
#include "aktualizr-lite/aklite_client_ext.h"
#include "crypto/keymanager.h"
#include "helpers.h"
#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "storage/invstorage.h"
#include "target.h"
#include "utilities/aktualizr_version.h"

int daemon_main(LiteClient& client, const bpo::variables_map& variables_map);
