#include "inode_fs.hpp"
#include "storage.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Action>
void rejected(Action action, const char* message) {
    try { action(); } catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path folder(argv[1]);
    std::filesystem::create_directories(folder);
    const auto image = folder / "storage-check.img";
    if (std::filesystem::exists(image)) {
        std::cerr << "Use a fresh test directory.\n";
        return 2;
    }
    try {
        Bytes original(9001, 17);
        {
            InodeFs disk(image, 4 * 1024 * 1024);
            disk.mkdir("/one");
            disk.mkdir("/one/two");
            disk.write("/one/two/file", original);
            require(disk.read("/one/two/file") == original, "multi-block read mismatch");
            const auto before = disk.statistics().free_blocks;
            rejected([&] { disk.write("/one/two/file", Bytes(4 * 1024 * 1024, 99)); }, "disk full accepted");
            require(disk.read("/one/two/file") == original, "failed write changed file");
            require(disk.statistics().free_blocks == before, "failed write leaked blocks");
            rejected([&] { disk.remove("/one"); }, "non-empty directory removed");
            rejected([&] { disk.write("/one/../escape", {}); }, "path traversal accepted");
            rejected([&] { InodeFs second(image); }, "same image opened concurrently");
            disk.write("/empty", {});
            require(disk.read("/empty").empty(), "empty file read mismatch");
            disk.write("/one/two/file", Bytes(21, 42));
            require(disk.statistics().free_blocks == before + 2, "overwrite did not reclaim old blocks");
            disk.remove("/one/two/file");
            require(disk.statistics().free_blocks == before + 3, "delete did not reclaim blocks");
            disk.remove("/one/two");
            std::vector<std::thread> writers;
            for (unsigned i = 0; i < 4; ++i) writers.emplace_back([&, i] {
                disk.write("/thread-" + std::to_string(i), Bytes(5000, static_cast<std::uint8_t>(i)));
            });
            for (auto& writer : writers) writer.join();
        }
        {
            InodeFs reopened(image);
            require(reopened.exists("/one"), "directory lost on reopen");
            require(!reopened.exists("/one/two"), "deleted directory reappeared");
            for (unsigned i = 0; i < 4; ++i) {
                require(reopened.read("/thread-" + std::to_string(i)) == Bytes(5000, static_cast<std::uint8_t>(i)),
                        "concurrent write or reopen mismatch");
            }
        }
        std::string id;
        {
            JobStore jobs(folder / "jobs");
            id = jobs.create_job(original, "{}");
            require(jobs.owner(id) == "admin", "default job owner mismatch");
            jobs.save_tile(id, 0, original);
            rejected([&] { jobs.download(id); }, "incomplete job download accepted");
            jobs.finish(id, original, "{\"state\":\"complete\"}");
            jobs.abort(id);
            require(jobs.download(id) == original, "job result mismatch");
            rejected([&] { jobs.save_tile(id, 0, {}); }, "completed job changed");
        }
        {
            JobStore reopened(folder / "jobs");
            require(reopened.download(id) == original, "job result lost on reopen");
            require(!reopened.status(id).empty(), "job status lost on reopen");
            require(reopened.owner(id) == "admin", "job owner lost on reopen");
        }
        {
            JobStore jobs(folder / "tile-jobs");
            const Image source{65, 1, Bytes(65 * 3, 27)};
            rejected([&] { jobs.create_job(encode_ppm(source), "{}", "mallory"); }, "unknown owner accepted");
            const auto tile_job = jobs.create_job(encode_ppm(source), "{}", "alice");
            require(jobs.owner(tile_job) == "alice", "explicit job owner mismatch");
            jobs.save_tile(tile_job, 1, Bytes{1, 2, 3});
            const auto result = decode_ppm(jobs.tile(tile_job, 1));
            require(result.width == 1 && result.height == 1 && result.pixels == Bytes({1, 2, 3}),
                    "committed edge tile mismatch");
            rejected([&] { jobs.tile(tile_job, 2); }, "unknown tile accepted");
            rejected([&] { jobs.tile(tile_job, 0); }, "uncommitted tile accepted");
            jobs.save_tile(tile_job, 1, Bytes{1});
            rejected([&] { jobs.tile(tile_job, 1); }, "invalid tile pixel length accepted");
            jobs.abort(tile_job);
        }
        {
            JobStore jobs(folder / "abort-jobs");
            for (unsigned i = 0; i < 5; ++i) {
                const auto failed = jobs.create_job(original, "{}");
                jobs.save_tile(failed, 0, original);
                jobs.save_tile(failed, 1, Bytes(5000, 1));
                rejected([&] { jobs.finish(failed, original, std::string(max_frame + 1, 'x')); },
                         "oversized final metadata accepted");
                rejected([&] { jobs.download(failed); }, "partially finished job download accepted");
                jobs.abort(failed);
                jobs.abort(failed);
            }
            rejected([&] { jobs.create_job(original, std::string(max_frame + 1, 'x')); },
                     "oversized job metadata accepted");
        }
        {
            InodeFs disk(folder / "abort-jobs" / "disk.img");
            const auto statistics = disk.statistics();
            require(statistics.used_inodes == 2, "abort or failed create leaked inodes");
            require(statistics.free_blocks == statistics.data_blocks, "abort or failed create leaked blocks");
        }
        std::cout << "PASS: inode directories, block allocation/reclamation, disk-full rollback, image lock, concurrent writes, reopen, job commit marker, failed-job cleanup\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
