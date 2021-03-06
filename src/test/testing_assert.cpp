#include "testing_assert.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "all_type_variant.hpp"
#include "constant_mappings.hpp"
#include "logical_query_plan/abstract_lqp_node.hpp"
#include "logical_query_plan/join_node.hpp"
#include "storage/table.hpp"
#include "storage/value_column.hpp"

#define ANSI_COLOR_RED "\x1B[31m"
#define ANSI_COLOR_GREEN "\x1B[32m"
#define ANSI_COLOR_BG_RED "\x1B[41m"
#define ANSI_COLOR_BG_GREEN "\x1B[42m"
#define ANSI_COLOR_RESET "\x1B[0m"

#define EPSILON 0.0001

namespace {

using Matrix = std::vector<std::vector<opossum::AllTypeVariant>>;

Matrix _table_to_matrix(const std::shared_ptr<const opossum::Table>& table) {
  // initialize matrix with table sizes, including column names/types
  Matrix matrix(table->row_count() + 2, std::vector<opossum::AllTypeVariant>(table->column_count()));

  // set column names/types
  for (auto column_id = opossum::ColumnID{0}; column_id < table->column_count(); ++column_id) {
    matrix[0][column_id] = table->column_name(column_id);
    matrix[1][column_id] = opossum::data_type_to_string.left.at(table->column_type(column_id));
  }

  // set values
  unsigned row_offset = 0;
  for (auto chunk_id = opossum::ChunkID{0}; chunk_id < table->chunk_count(); chunk_id++) {
    auto chunk = table->get_chunk(chunk_id);

    // an empty table's chunk might be missing actual columns
    if (chunk->size() == 0) continue;

    for (auto column_id = opossum::ColumnID{0}; column_id < table->column_count(); ++column_id) {
      const auto column = chunk->get_column(column_id);

      for (auto chunk_offset = opossum::ChunkOffset{0}; chunk_offset < chunk->size(); ++chunk_offset) {
        matrix[row_offset + chunk_offset + 2][column_id] = (*column)[chunk_offset];
      }
    }
    row_offset += chunk->size();
  }

  return matrix;
}

std::string _matrix_to_string(const Matrix& matrix, const std::vector<std::pair<uint64_t, uint16_t>>& highlight_cells,
                              const std::string& highlight_color, const std::string& highlight_color_bg) {
  std::stringstream stream;
  bool previous_row_highlighted = false;

  for (auto row_id = size_t{0}; row_id < matrix.size(); row_id++) {
    auto highlight = false;
    auto it = std::find_if(highlight_cells.begin(), highlight_cells.end(),
                           [&](const auto& element) { return element.first == row_id; });
    if (it != highlight_cells.end()) {
      highlight = true;
      if (!previous_row_highlighted) {
        stream << "<<<<<" << std::endl;
        previous_row_highlighted = true;
      }
    } else {
      previous_row_highlighted = false;
    }

    // Highlight row number with background color
    auto coloring = std::string{};
    if (highlight) {
      coloring = highlight_color_bg;
    }
    if (row_id >= 2) {
      stream << coloring << std::setw(4) << std::to_string(row_id - 2) << ANSI_COLOR_RESET;
    } else {
      stream << coloring << std::setw(4) << "    " << ANSI_COLOR_RESET;
    }

    // Highlicht each (applicable) cell with highlight color
    for (auto column_id = opossum::ColumnID{0}; column_id < matrix[row_id].size(); column_id++) {
      auto cell = boost::lexical_cast<std::string>(matrix[row_id][column_id]);
      coloring = "";
      if (highlight && it->second == column_id) {
        coloring = highlight_color;
      }
      stream << coloring << std::setw(8) << cell << ANSI_COLOR_RESET << " ";
    }
    stream << std::endl;
  }
  return stream.str();
}

template <typename T>
bool almost_equals(T left_val, T right_val, opossum::FloatComparisonMode float_comparison_mode) {
  static_assert(std::is_floating_point_v<T>, "Values must be of floating point type.");
  if (float_comparison_mode == opossum::FloatComparisonMode::AbsoluteDifference) {
    return std::fabs(left_val - right_val) < EPSILON;
  } else {
    return std::fabs(left_val - right_val) < std::fabs(right_val * EPSILON);
  }
}

}  // namespace

namespace opossum {

bool check_table_equal(const std::shared_ptr<const Table>& opossum_table,
                       const std::shared_ptr<const Table>& expected_table, OrderSensitivity order_sensitivity,
                       TypeCmpMode type_cmp_mode, FloatComparisonMode float_comparison_mode) {
  auto opossum_matrix = _table_to_matrix(opossum_table);
  auto expected_matrix = _table_to_matrix(expected_table);

  const auto print_table_comparison = [&](const std::string& error_type, const std::string& error_msg,
                                          const std::vector<std::pair<uint64_t, uint16_t>>& highlighted_cells = {}) {
    std::cout << "========= Tables are not equal =========" << std::endl;
    std::cout << "------- Actual Result -------" << std::endl;
    std::cout << _matrix_to_string(opossum_matrix, highlighted_cells, ANSI_COLOR_RED, ANSI_COLOR_BG_RED);
    std::cout << "-----------------------------" << std::endl << std::endl;
    std::cout << "------- Expected Result -------" << std::endl;
    std::cout << _matrix_to_string(expected_matrix, highlighted_cells, ANSI_COLOR_GREEN, ANSI_COLOR_BG_GREEN);
    std::cout << "-------------------------------" << std::endl;
    std::cout << "========================================" << std::endl << std::endl;
    std::cout << "Type of error: " << error_type << std::endl;
    std::cout << error_msg << std::endl << std::endl;
  };

  // compare schema of tables
  //  - column count
  if (opossum_table->column_count() != expected_table->column_count()) {
    const std::string error_type = "Column count mismatch";
    const std::string error_msg = "Actual number of columns: " + std::to_string(opossum_table->column_count()) + "\n" +
                                  "Expected number of columns: " + std::to_string(expected_table->column_count());

    print_table_comparison(error_type, error_msg);
    return false;
  }

  //  - column names and types
  DataType left_col_type, right_col_type;
  for (auto column_id = ColumnID{0}; column_id < expected_table->column_count(); ++column_id) {
    left_col_type = opossum_table->column_type(column_id);
    right_col_type = expected_table->column_type(column_id);
    // This is needed for the SQLiteTestrunner, since SQLite does not differentiate between float/double, and int/long.
    if (type_cmp_mode == TypeCmpMode::Lenient) {
      if (left_col_type == DataType::Double) {
        left_col_type = DataType::Float;
      } else if (left_col_type == DataType::Long) {
        left_col_type = DataType::Int;
      }

      if (right_col_type == DataType::Double) {
        right_col_type = DataType::Float;
      } else if (right_col_type == DataType::Long) {
        right_col_type = DataType::Int;
      }
    }

    if (opossum_table->column_name(column_id) != expected_table->column_name(column_id)) {
      const std::string error_type = "Column name mismatch (column " + std::to_string(column_id) + ")";
      const std::string error_msg = "Actual column name: " + opossum_table->column_name(column_id) + "\n" +
                                    "Expected column name: " + expected_table->column_name(column_id);

      print_table_comparison(error_type, error_msg, {{0, column_id}});
      return false;
    }

    if (left_col_type != right_col_type) {
      const std::string error_type = "Column type mismatch (column " + std::to_string(column_id) + ")";
      const std::string error_msg =
          "Actual column type: " + data_type_to_string.left.at(opossum_table->column_type(column_id)) + "\n" +
          "Expected column type: " + data_type_to_string.left.at(expected_table->column_type(column_id));

      print_table_comparison(error_type, error_msg, {{1, column_id}});
      return false;
    }
  }

  // compare content of tables
  //  - row count for fast failure
  if (opossum_table->row_count() != expected_table->row_count()) {
    const std::string error_type = "Row count mismatch";
    const std::string error_msg = "Actual number of rows: " + std::to_string(opossum_table->row_count()) + "\n" +
                                  "Expected number of rows: " + std::to_string(expected_table->row_count());

    print_table_comparison(error_type, error_msg);
    return false;
  }

  // sort if order does not matter
  if (order_sensitivity == OrderSensitivity::No) {
    // skip header when sorting
    std::sort(opossum_matrix.begin() + 2, opossum_matrix.end());
    std::sort(expected_matrix.begin() + 2, expected_matrix.end());
  }

  bool has_error = false;
  std::vector<std::pair<uint64_t, uint16_t>> mismatched_cells{};

  const auto highlight_if = [&has_error, &mismatched_cells](bool statement, uint64_t row_id, uint16_t column_id) {
    if (statement) {
      has_error = true;
      mismatched_cells.push_back({row_id, column_id});
    }
  };

  // Compare each cell, skipping header
  for (auto row_id = size_t{2}; row_id < opossum_matrix.size(); row_id++)
    for (auto column_id = ColumnID{0}; column_id < opossum_matrix[row_id].size(); column_id++) {
      if (variant_is_null(opossum_matrix[row_id][column_id]) || variant_is_null(expected_matrix[row_id][column_id])) {
        highlight_if(!(variant_is_null(opossum_matrix[row_id][column_id]) &&
                       variant_is_null(expected_matrix[row_id][column_id])),
                     row_id, column_id);
      } else if (opossum_table->column_type(column_id) == DataType::Float) {
        auto left_val = type_cast<float>(opossum_matrix[row_id][column_id]);
        auto right_val = type_cast<float>(expected_matrix[row_id][column_id]);

        highlight_if(!almost_equals(left_val, right_val, float_comparison_mode), row_id, column_id);
      } else if (opossum_table->column_type(column_id) == DataType::Double) {
        auto left_val = type_cast<double>(opossum_matrix[row_id][column_id]);
        auto right_val = type_cast<double>(expected_matrix[row_id][column_id]);

        highlight_if(!almost_equals(left_val, right_val, float_comparison_mode), row_id, column_id);
      } else {
        if (type_cmp_mode == TypeCmpMode::Lenient && (opossum_table->column_type(column_id) == DataType::Int ||
                                                      opossum_table->column_type(column_id) == DataType::Long)) {
          auto left_val = type_cast<int64_t>(opossum_matrix[row_id][column_id]);
          auto right_val = type_cast<int64_t>(expected_matrix[row_id][column_id]);
          highlight_if(left_val != right_val, row_id, column_id);
        } else {
          highlight_if(opossum_matrix[row_id][column_id] != expected_matrix[row_id][column_id], row_id, column_id);
        }
      }
    }

  if (has_error) {
    const std::string error_type = "Cell data mismatch";
    std::string error_msg = "Mismatched cells (row,column): ";
    for (auto cell : mismatched_cells) {
      error_msg += "(" + std::to_string(cell.first - 2) + "," + std::to_string(cell.second) + ") ";
    }

    print_table_comparison(error_type, error_msg, mismatched_cells);
    return false;
  }

  return true;
}

void ASSERT_INNER_JOIN_NODE(const std::shared_ptr<AbstractLQPNode>& node, ScanType scan_type,
                            const LQPColumnReference& left_column_reference,
                            const LQPColumnReference& right_column_reference) {
  ASSERT_EQ(node->type(), LQPNodeType::Join);  // Can't cast otherwise
  auto join_node = std::dynamic_pointer_cast<JoinNode>(node);
  ASSERT_EQ(join_node->join_mode(), JoinMode::Inner);  // Can't access join_column_ids() otherwise
  EXPECT_EQ(join_node->scan_type(), scan_type);
  EXPECT_EQ(join_node->join_column_references(), std::make_pair(left_column_reference, right_column_reference));
}

void ASSERT_CROSS_JOIN_NODE(const std::shared_ptr<AbstractLQPNode>& node) {}

bool check_lqp_tie(const std::shared_ptr<const AbstractLQPNode>& parent, LQPChildSide child_side,
                   const std::shared_ptr<const AbstractLQPNode>& child) {
  auto parents = child->parents();
  for (const auto& parent2 : parents) {
    if (!parent2) {
      return false;
    }
    if (parent == parent2 && parent2->child(child_side) == child) {
      return true;
    }
  }

  return false;
}

bool subtree_types_are_equal(const std::shared_ptr<AbstractLQPNode>& got,
                             const std::shared_ptr<AbstractLQPNode>& expected) {
  if (got == nullptr && expected == nullptr) return true;
  if (got == nullptr) return false;
  if (expected == nullptr) return false;

  if (got->type() != expected->type()) return false;
  return subtree_types_are_equal(got->left_child(), expected->left_child()) &&
         subtree_types_are_equal(got->right_child(), expected->right_child());
}

}  // namespace opossum
