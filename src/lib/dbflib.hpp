#include <vector>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdint>

/*
 * Dynamically linked binary file library
 */
namespace dbflib {
    // do not change this value
    constexpr uint64_t DB_FILE_MAGIC = 0x0d0a46424424;
    // minimum support version 
    constexpr uint8_t DB_FILE_MIN_VERSION = 0x10;
    // current version
    constexpr uint8_t DB_FILE_CURR_VERSION = 0x10;

    static_assert(DB_FILE_MIN_VERSION <= DB_FILE_CURR_VERSION && "Minimum version should be lower or equal to the current version");

    enum DB_FILE_VERSION_FEATURE : uint8_t {
        LINKING = 0x10,
    };

    typedef uint32_t BlockId;
    typedef uint32_t BlockOffset;
    typedef uint32_t BlockSize;

    struct DB_FILE {
        uint8_t magic[sizeof(decltype(DB_FILE_MAGIC))]{};
        uint8_t version{};
        uint8_t flags{};
        uint16_t links_count{};
        uint32_t links_table_offset{};
        uint32_t start_offset{};
        uint32_t data_size{};
        uint32_t file_size{};

        constexpr void* Start() {
            return (void*)(magic + start_offset);
        }
    };

    struct DB_FILE_LINK {
        uint32_t origin;
        uint32_t destination;
    };

    class DBFileBuilder {
        bool linked{};
        std::vector<uint8_t> data{};
        std::unordered_map<BlockId, BlockSize> blocks{};
        std::vector<DB_FILE_LINK> links{};

        DB_FILE* Header() {
            return reinterpret_cast<DB_FILE*>(data.data());
        }

        inline void AssertNotLinked() {
#ifdef DEBUG
            if (linked) {
                throw std::runtime_error("builder already linked!");
            }
#endif
            linked = false;
        }
    public:
        DBFileBuilder() {
            data.resize(sizeof(DBFileBuilder));
            Header()->start_offset = (uint32_t)data.size();
        }

        /*
         * Create a block from a buffer.
         * @param buffer buffer, should contain at least len bytes
         * @param len length of the buffer
         * @return block id
         */
        BlockId CreateBlock(void* buffer, size_t len) {
            AssertNotLinked();
            size_t id = data.size();
            if (len) {
                if (id + len > INT32_MAX) {
                    throw std::runtime_error("file too big");
                }
                data.insert(data.end(), (uint8_t*)buffer, (uint8_t*)buffer + len);
                blocks[(BlockId)id] = (BlockSize)len;
            }
            return (BlockId)id;
        }

        /*
         * Create a block inside the file.
         * @param BlockType pointer type to return
         * @param len length of the buffer to create
         * @return block id and pointer, the pointer is valid until a new block is created
         */
        template<typename BlockType = void>
        std::pair<BlockId, BlockType*> CreateBlock(const size_t len = sizeof(BlockType)) {
            AssertNotLinked();
            size_t id = data.size();
            if (len) {
                if (id + len > INT32_MAX) {
                    throw std::runtime_error("file too big");
                }
                data.resize(id + len);
                blocks[(BlockId)id] = (BlockSize)len;
            }
            return std::make_pair((BlockId)id, reinterpret_cast<BlockType*>(&data[id]));
        }

        /*
         * Get a block inside the file.
         * @param BlockType pointer type to return
         * @param id block id
         * @return block pointer, the pointer is valid until a new block is created
         */
        template<typename BlockType = void>
        BlockType* GetBlock(BlockId id) {
            if (id > data.size()) {
                throw std::runtime_error("invalid block");
            }
            return reinterpret_cast<BlockType*>(&data[id]);
        }

        /*
         * Get a block size.
         * @param id block id
         * @return block size
         */
        BlockSize GetBlockSize(BlockId id) const {
            auto it = blocks.find(id);
            if (it == blocks.end()) {
                return 0;
            }
            return it->second;
        }

        /*
         * Create a link between 2 locations.
         * @param blockOrigin origin block id
         * @param origin origin offset
         * @param blockDestination destination block id
         * @param destination destination offset
         */
        void CreateLink(BlockId blockOrigin, BlockOffset origin, BlockId blockDestination, BlockOffset destination = 0) {
            AssertNotLinked();
            BlockSize ors = GetBlockSize(blockOrigin);
            BlockSize dss = GetBlockSize(blockDestination);

            if (origin + 8 > ors || destination > dss) {
                throw std::runtime_error("trying to create a link after the end of a block");
            }
            
            links.emplace_back((uint32_t)(blockOrigin + origin), (uint32_t)(blockDestination + destination));
        }

        /*
         * Build the file and return the start
         * @return file
         */
        DB_FILE* Build() {
            if (linked) {
                return Header();
            }
            linked = true;
            size_t dataSize{ data.size() - Header()->start_offset };
            size_t linksOffset{ data.size() };
            if (!links.empty()) {
                // insert links
                size_t len = sizeof(links[0]) * links.size();
                if (linksOffset + len > INT32_MAX) {
                    throw std::runtime_error("file too big");
                }
                data.insert(data.end(), reinterpret_cast<uint8_t*>(links.data()), reinterpret_cast<uint8_t*>(links.data()) + len);
            }

            DB_FILE* header = Header();

            *reinterpret_cast<uint64_t*>(header->magic) = DB_FILE_MAGIC;
            header->version = DB_FILE_CURR_VERSION;
            header->links_table_offset = (uint32_t)linksOffset;
            header->links_count = (uint32_t)links.size();
            header->data_size = (uint32_t)dataSize;
            header->file_size = (uint32_t)data.size();

            return header;
        }

        /*
         * Build the file and write it into a path
         */
        void WriteToFile(const std::filesystem::path& path) {
            DB_FILE* f = Build();

            std::ofstream of{ path, std::ios::binary };

            if (!of) {
                throw std::runtime_error("can't open output file");
            }

            of.write((const char*)f, f->file_size);

            of.close();
        }
    };

    class DBFileReader {
        std::string readData{};
        DB_FILE* file;

        void ValidateAndLink(size_t len = 0) {
            if (len && len < offsetof(DB_FILE, file_size) + sizeof(sizeof(file->file_size))) {
                throw std::runtime_error("invalid file: file too small");
            }

            if (*reinterpret_cast<decltype(DB_FILE_MAGIC)*>(file->magic) != DB_FILE_MAGIC) {
                throw std::runtime_error("invalid file: bad magic");
            }

            if (file->version < DB_FILE_MIN_VERSION) {
                throw std::runtime_error("invalid file: version too low");
            }

            if (len && file->file_size > len) {
                throw std::runtime_error("invalid file: read file too small");
            }

            if (file->start_offset > file->file_size) {
                throw std::runtime_error("invalid file: start offset after file end");
            }

            if (file->version >= DB_FILE_VERSION_FEATURE::LINKING) {
                DB_FILE_LINK* links = reinterpret_cast<DB_FILE_LINK*>(file->magic + file->links_table_offset);
                for (size_t i = 0; i < file->links_count; i++) {
                    DB_FILE_LINK& link = links[i];

                    if (link.origin > file->file_size) throw std::runtime_error("invalid file: link after end file");
                    if (link.destination > file->file_size) throw std::runtime_error("invalid file: link after end file");

                    *reinterpret_cast<void**>(file->magic + link.origin) = file->magic + link.destination;
                }
            }
        }
    public:

        /*
         * Create a reader from a file
         * @param path path
         */
        DBFileReader(const std::filesystem::path& path) {
            std::ifstream in{ path, std::ios::binary };
            if (!in) {
                throw std::runtime_error("can't open input file");
            }

            in.seekg(0, std::ios::end);
            size_t length = in.tellg();
            in.seekg(0, std::ios::beg);

            readData.resize(length);

            in.read(readData.data(), length);

            in.close();

            file = reinterpret_cast<DB_FILE*>(readData.data());
            ValidateAndLink(length);
        }

        /*
         * Create a reader from a buffer
         * @param buffer buffer
         * @param length buffer size, 0 for unknown
         */
        DBFileReader(void* buffer, size_t length = 0) : file((DB_FILE*)buffer) {
            ValidateAndLink(length);
        }

        /*
         * Get file data
         */
        constexpr DB_FILE* GetFile() {
            return file;
        }

        /*
         * Get start data
         */
        constexpr void* GetStart() {
            return file->magic + file->start_offset;
        }
    };
}