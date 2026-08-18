#pragma once

#include <cstddef>
#include <string>

class SmManager;

namespace rmdb {

size_t load_csv_into_table(SmManager *sm_manager, const std::string &tab_name, const std::string &file_name);

}  // namespace rmdb
