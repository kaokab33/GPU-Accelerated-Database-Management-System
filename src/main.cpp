#include "duckdb.hpp"
#include "dbms/csv_importer.hpp"
#include "dbms/duckdb_interface.hpp"
#include "dbms/profiler.hpp"
#include "constants/db.hpp"
#include "physical_plan/physical_op.hpp"
#include "duckdb/execution/executor.hpp"
#include <memory>
#include <fstream>
#include <iostream>
#include <string>

void printPhysicalPlan(duckdb::PhysicalOperator *op, int indent)
{
    if (!op)
        return;

    std::string pad(indent, ' ');
    std::cout << pad << "- " << op->GetName() << "\n";
    auto m = op->ParamsToString();

    // if (op->type == duckdb::PhysicalOperatorType::TABLE_SCAN){
    //     auto &table_scan = op->Cast<duckdb::PhysicalTableScan>();
    // }
    // else if (op->type == duckdb::PhysicalOperatorType::PROJECTION)
    // {
    //     auto &projection = op->Cast<duckdb::PhysicalProjection>();
    //     std::cout << pad << "  Projection: ";
    //     for (auto &expr : projection.expressions)
    //     {
    //         std::cout << expr->ToString() << ", ";
    //     }
    //     std::cout << "\n";
    // }

    if (m.size() > 0)
    {
        std::cout << pad << "  Params: ";
        for (auto &pair : m)
        {
            std::cout << pair.first << ": " << pair.second << ", ";
        }
        std::cout << "\n";
    }
    for (auto &child : op->children)
    {
        printPhysicalPlan(&(child.get()), indent + 2);
    }
}
std::string readQueryFromFile(const std::string &filename)
{
    std::ifstream inputFile(filename);
    if (!inputFile.is_open())
    {
        throw std::runtime_error("Error opening file: " + filename);
    }

    std::string query;
    std::string line;
    while (std::getline(inputFile, line))
    {
        if (!query.empty())
        {
            query += " ";
        }
        query += line;
    }

    inputFile.close();
    return query;
}

void setupDatabase(duckdb::Connection &con)
{
    con.Query("SET disabled_optimizers = 'filter_pushdown, statistics_propagation';");
    con.Query("SET scalar_subquery_error_on_multiple_rows=false;");
    con.Query("SET disabled_optimizers = 'late_materialization,compressed_materialization,unused_columns,column_lifetime,statistics_propagation,filter_pushdown,common_aggregate';");

    con.Query("SET threads TO 1;");
}

bool importData(CSVImporter &csv_importer, DB &data_base, std::string data_dir = "data/")
{
    bool success = csv_importer.import_folder(data_dir, &data_base);
    if (!success)
    {
        std::cerr << "Error importing CSV folder." << std::endl;
        return false;
    }
    std::cout << "CSV folder imported successfully." << std::endl;
    return true;
}

void executeQueryCPU(duckdb::PhysicalOperator *physical_plan, duckdb::Connection &con, DB &data_base, Profiler &profiler, std::string query, std::string data_dir)
{
    profiler.start("CPU Execution");
    for (const auto &table : data_base.tables)
    {
        std::string table_name = table.name;
        std::string csv_file = data_dir + "/" + table_name + ".csv";
        std::string create_table_query = "COPY " + std::string(table_name) + " FROM '" + csv_file + "';";
        auto result = con.Query(create_table_query);
        if (result->HasError())
        {
            std::cerr << "Error executing query: " << result->GetError() << std::endl;
        }
    }
    auto result = con.Query(query);

    if (result->HasError())
    {
        std::cerr << "Error executing query: " << result->GetError() << std::endl;
    }
    profiler.stop("CPU Execution");
}

void executeQueryGPU(duckdb::PhysicalOperator *physical_plan, DB &data_base, Profiler &profiler, std::string query_file_name, std::string data_dir)
{
    profiler.start("GPU Execution");
    PhysicalOpNode::executePlanInBatches(physical_plan, &data_base, query_file_name, data_dir, 50000, true);
    profiler.stop("GPU Execution");
}

int main(int argc, char *argv[])
{
    // if (argc != 3)
    // {
    //     std::cerr << "Usage: " << argv[0] << " <data_folder_path> <query_file>" << std::endl;
    //     return 1;
    // }

    std::string data_folder_path ="data_small/";
    std::string query_file = "queries_small/query3.txt";
    try
    {
        Profiler profiler;
        profiler.start("Total");

        DB data_base;
        duckdb::DuckDB db(nullptr);
        duckdb::Connection con(db);
        DuckDBInterface duckdb_interface(db, con);

        setupDatabase(con);

        profiler.start("Import CSV");
        CSVImporter csv_importer(db, con);
        if (!importData(csv_importer, data_base, data_folder_path))
        {
            return 1;
        }
        con.BeginTransaction();
        profiler.stop("Import CSV");

        std::string query = readQueryFromFile(query_file);

        profiler.start("Get Logical Plan");
        auto logical_plan = duckdb_interface.getLogicalPlan(query);
        profiler.stop("Get Logical Plan");

        profiler.start("Get Physical Plan");
        duckdb::PhysicalPlanGenerator physical_plan_generator(*con.context);
        auto physical_plan = physical_plan_generator.Plan(logical_plan->Copy(*con.context));
        profiler.stop("Get Physical Plan");
        // std::cout << physical_plan.get()->Root().ToString() << std::endl;

        // executeQueryCPU(&(physical_plan.get()->Root()), con, data_base, profiler, query, data_folder_path);
        // printPhysicalPlan(&(physical_plan.get()->Root()), 0);
        executeQueryGPU(&(physical_plan.get()->Root()), data_base, profiler, query_file, data_folder_path);

        profiler.stop("Total");
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    catch (...)
    {
        std::cerr << "Unknown error occurred" << std::endl;
        return 1;
    }
}