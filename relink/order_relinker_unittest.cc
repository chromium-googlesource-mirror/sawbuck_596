// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/relink/order_relinker.h"

#include "base/file_util.h"
#include "gtest/gtest.h"
#include "syzygy/core/unittest_util.h"
#include "syzygy/pe/decomposer.h"
#include "syzygy/pe/pe_file.h"
#include "syzygy/pe/unittest_util.h"
#include "syzygy/reorder/random_order_generator.h"

namespace relink {

using reorder::RandomOrderGenerator;
using reorder::Reorderer;

class OrderRelinkerTest : public testing::PELibUnitTest {
 protected:
  static const FilePath kInstrDllName;
  static const FilePath kOrderFileName;
  static const FilePath kTestDataDir;
};

const FilePath OrderRelinkerTest::kOrderFileName(L"order_file.json");
const FilePath OrderRelinkerTest::kInstrDllName(L"instrumented_test_dll.dll");
const FilePath OrderRelinkerTest::kTestDataDir(L"test_data");

TEST_F(OrderRelinkerTest, Relink) {
  FilePath temp_dir;
  ASSERT_NO_FATAL_FAILURE(CreateTemporaryDir(&temp_dir));
  FilePath output_dll_path = temp_dir.Append(kDllName);
  FilePath output_pdb_path = temp_dir.Append(kDllPdbName);
  FilePath order_file_path = temp_dir.Append(kOrderFileName);

  FilePath test_data_dir =
      testing::GetExeRelativePath(kTestDataDir.value().c_str());
  FilePath input_dll_path = test_data_dir.Append(kDllName);
  FilePath input_pdb_path = test_data_dir.Append(kDllPdbName);
  FilePath instr_dll_path = test_data_dir.Append(kInstrDllName);

  pe::PEFile pe_file;
  block_graph::BlockGraph block_graph;
  pe::ImageLayout image_layout(&block_graph);
  Reorderer::Order order;
  RandomOrderGenerator order_generator(12345);
  std::vector<FilePath> trace_paths;
  Reorderer::Flags flags = Reorderer::kFlagReorderCode |
      Reorderer::kFlagReorderData;
  Reorderer reorderer(input_dll_path,
                      instr_dll_path,
                      trace_paths,
                      flags);
  ASSERT_TRUE(reorderer.Reorder(&order_generator,
                                &order,
                                &pe_file,
                                &image_layout));
  ASSERT_TRUE(order.SerializeToJSON(pe_file, order_file_path, true));

  OrderRelinker relinker(order_file_path);
  ASSERT_TRUE(relinker.Relink(input_dll_path,
                              input_pdb_path,
                              output_dll_path,
                              output_pdb_path,
                              true));
  ASSERT_NO_FATAL_FAILURE(CheckTestDll(output_dll_path));

  CheckEmbeddedPdbPath(output_dll_path, output_pdb_path);
}

TEST_F(OrderRelinkerTest, RelinkWithPadding) {
  FilePath temp_dir;
  ASSERT_NO_FATAL_FAILURE(CreateTemporaryDir(&temp_dir));
  FilePath output_dll_path = temp_dir.Append(kDllName);
  FilePath output_pdb_path = temp_dir.Append(kDllPdbName);
  FilePath order_file_path = temp_dir.Append(kOrderFileName);

  FilePath test_data_dir =
      testing::GetExeRelativePath(kTestDataDir.value().c_str());
  FilePath input_dll_path = test_data_dir.Append(kDllName);
  FilePath input_pdb_path = test_data_dir.Append(kDllPdbName);
  FilePath instr_dll_path = test_data_dir.Append(kInstrDllName);

  pe::PEFile pe_file;
  block_graph::BlockGraph block_graph;
  pe::ImageLayout image_layout(&block_graph);
  Reorderer::Order order;
  RandomOrderGenerator order_generator(12345);
  std::vector<FilePath> trace_paths;
  Reorderer::Flags flags = Reorderer::kFlagReorderCode |
      Reorderer::kFlagReorderData;
  Reorderer reorderer(input_dll_path,
                      instr_dll_path,
                      trace_paths,
                      flags);
  ASSERT_TRUE(reorderer.Reorder(&order_generator,
                                &order,
                                &pe_file,
                                &image_layout));
  ASSERT_TRUE(order.SerializeToJSON(pe_file, order_file_path, true));

  OrderRelinker relinker(order_file_path);
  relinker.set_padding_length(32);
  ASSERT_TRUE(relinker.Relink(input_dll_path,
                              input_pdb_path,
                              output_dll_path,
                              output_pdb_path,
                              true));
  ASSERT_NO_FATAL_FAILURE(CheckTestDll(output_dll_path));

  CheckEmbeddedPdbPath(output_dll_path, output_pdb_path);
}

}  // namespace relink
