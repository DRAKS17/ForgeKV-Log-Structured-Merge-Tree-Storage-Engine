#include "forgekv.h"

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void print_help() {
    std::cout << "\nCommands:\n"
              << "  PUT <key> <value>         Insert or update a key\n"
              << "  GET <key>                 Look up a key\n"
              << "  DEL <key>                 Delete a key\n"
              << "  SCAN <start> <end>        Range query [start, end]\n"
              << "  COMPACT                   Flush & merge all SSTables\n"
              << "  STATS                     Show MemTable, SSTable & Bloom stats\n"
              << "  HELP                      Show this help menu\n"
              << "  EXIT / QUIT               Close database and exit\n\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string db_dir = (argc > 1) ? argv[1] : "data/cli_db";

    forgekv::ForgeKV db;
    try {
        db.open(db_dir, 64 * 1024);
    } catch (const std::exception& ex) {
        std::cerr << "Error opening database at '" << db_dir << "': " << ex.what() << "\n";
        return 1;
    }

    std::cout << "ForgeKV Interactive Shell (data dir: " << db_dir << ")\n"
              << "Type HELP for commands.\n";

    std::string line;
    while (true) {
        std::cout << "forgekv> " << std::flush;
        if (!std::getline(std::cin, line)) {
            break;
        }

        std::istringstream iss(line);
        std::string cmd;
        if (!(iss >> cmd)) {
            continue;
        }
        for (char& c : cmd) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        try {
            if (cmd == "EXIT" || cmd == "QUIT") {
                break;
            } else if (cmd == "HELP") {
                print_help();
            } else if (cmd == "PUT") {
                std::string key;
                if (!(iss >> key)) {
                    std::cout << "Usage: PUT <key> <value>\n";
                    continue;
                }
                std::string value;
                std::getline(iss >> std::ws, value);
                db.put(key, value);
                std::cout << "OK\n";
            } else if (cmd == "GET") {
                std::string key;
                if (!(iss >> key)) {
                    std::cout << "Usage: GET <key>\n";
                    continue;
                }
                auto val = db.get(key);
                if (val.has_value()) {
                    std::cout << *val << "\n";
                } else {
                    std::cout << "(nil)\n";
                }
            } else if (cmd == "DEL") {
                std::string key;
                if (!(iss >> key)) {
                    std::cout << "Usage: DEL <key>\n";
                    continue;
                }
                db.del(key);
                std::cout << "OK\n";
            } else if (cmd == "SCAN") {
                std::string start_k, end_k;
                if (!(iss >> start_k >> end_k)) {
                    std::cout << "Usage: SCAN <start_key> <end_key>\n";
                    continue;
                }
                auto rows = db.scan(start_k, end_k);
                if (rows.empty()) {
                    std::cout << "(empty range)\n";
                } else {
                    for (const auto& [k, v] : rows) {
                        std::cout << k << " => " << v << "\n";
                    }
                }
            } else if (cmd == "COMPACT") {
                db.compact();
                std::cout << "Compaction complete.\n";
            } else if (cmd == "STATS") {
                auto st = db.stats();
                std::cout << "MemTable Entries : " << st.memtable_entries << "\n"
                          << "MemTable Bytes   : " << st.memtable_bytes << " B\n"
                          << "SSTable Count    : " << st.sstable_count << "\n"
                          << "Total Disk Bytes : " << st.total_disk_bytes << " B\n"
                          << "Bloom Skips      : " << st.total_bloom_hits << "\n"
                          << "SST Disk Reads   : " << st.total_disk_reads << "\n";
            } else {
                std::cout << "Unknown command '" << cmd << "'. Type HELP.\n";
            }
        } catch (const std::exception& ex) {
            std::cout << "Error: " << ex.what() << "\n";
        }
    }

    db.close();
    return 0;
}
