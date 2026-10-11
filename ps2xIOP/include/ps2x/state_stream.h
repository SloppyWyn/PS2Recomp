#pragma once

// Binary state streams for runner snapshots (docs/SNAPSHOT-DESIGN.md). Little-endian
// host layout. Every type written raw (pod and its containers) is recorded with its size
// and alignment, per section, so a later build can tell which sections it can still read
// (the type registry below); fields written one by one are checked by section length.
//
// Sections nest: beginSection("TAG ") ... endSection() writes the 4-byte tag and the
// section length, and the reader checks both, so a block that reads more or less than
// it wrote is reported at its own section instead of corrupting everything after it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2x
{
    class StateError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // Layout of a type written raw (pod): size and alignment, keyed by the compiler's type name.
    struct TypeLayout
    {
        uint32_t size = 0u;
        uint32_t align = 0u;
    };

    // Every type this build can write or read raw. Filled during static initialisation
    // (TypeRecord below is odr-used by every pod instantiation), so a loader knows the
    // current layouts without saving anything.
    inline std::map<std::string, TypeLayout> &typeRegistry()
    {
        static std::map<std::string, TypeLayout> registry;
        return registry;
    }

    inline bool registerType(const char *name, size_t size, size_t align)
    {
        typeRegistry()[name] = {static_cast<uint32_t>(size), static_cast<uint32_t>(align)};
        return true;
    }

    // Only aggregates and arrays are recorded: an arithmetic or enum value cannot change
    // layout without its section's field-by-field length changing too.
    template <typename T>
    inline constexpr bool kRecordedType = std::is_class_v<T> || std::is_union_v<T> || std::is_array_v<T>;

    template <typename T>
    struct TypeRecord
    {
        static inline const bool registered = registerType(typeid(T).name(), sizeof(T), alignof(T));
    };

    class StateWriter
    {
    public:
        void bytes(const void *data, size_t size)
        {
            if (size == 0u)
                return;
            const auto *begin = static_cast<const uint8_t *>(data);
            m_data.insert(m_data.end(), begin, begin + size);
        }

        template <typename T>
        void pod(const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            noteType<T>();
            bytes(&value, sizeof(T));
        }

        // Records that the current section holds T raw (also for an empty container, whose
        // elements would otherwise never be seen).
        template <typename T>
        void noteType()
        {
            if constexpr (kRecordedType<T>)
            {
                (void)TypeRecord<T>::registered;
                m_typeUse[typeid(T).name()].insert(m_path);
            }
        }

        void u8(uint8_t value) { pod(value); }
        void u16(uint16_t value) { pod(value); }
        void u32(uint32_t value) { pod(value); }
        void u64(uint64_t value) { pod(value); }
        void i32(int32_t value) { pod(value); }
        void i64(int64_t value) { pod(value); }
        void boolean(bool value) { u8(value ? 1u : 0u); }

        void size(size_t value) { u64(static_cast<uint64_t>(value)); }

        void str(std::string_view value)
        {
            size(value.size());
            bytes(value.data(), value.size());
        }

        template <typename T>
        void podVector(const std::vector<T> &values)
        {
            static_assert(std::is_trivially_copyable_v<T>, "podVector() needs a trivially copyable type");
            noteType<T>();
            size(values.size());
            bytes(values.data(), values.size() * sizeof(T));
        }

        template <typename Container>
        void podSequence(const Container &values)
        {
            noteType<typename Container::value_type>();
            size(values.size());
            for (const auto &value : values)
                pod(value);
        }

        void boolVector(const std::vector<bool> &values)
        {
            size(values.size());
            for (const bool value : values)
                boolean(value);
        }

        // Unordered maps are written in key order so equal states give equal files.
        template <typename Map, typename WriteValue>
        void sortedMap(const Map &map, WriteValue &&writeValue)
        {
            using Key = typename Map::key_type;
            noteType<Key>();
            std::vector<Key> keys;
            keys.reserve(map.size());
            for (const auto &entry : map)
                keys.push_back(entry.first);
            std::sort(keys.begin(), keys.end());
            size(keys.size());
            for (const Key &key : keys)
            {
                writeKey(key);
                writeValue(map.at(key));
            }
        }

        template <typename Map>
        void podMap(const Map &map)
        {
            noteType<typename Map::mapped_type>();
            sortedMap(map, [this](const auto &value)
                      { pod(value); });
        }

        template <typename Set>
        void sortedSet(const Set &set)
        {
            using Key = typename Set::key_type;
            noteType<Key>();
            std::vector<Key> keys(set.begin(), set.end());
            std::sort(keys.begin(), keys.end());
            size(keys.size());
            for (const Key &key : keys)
                writeKey(key);
        }

        void beginSection(const char (&tag)[5])
        {
            bytes(tag, 4u);
            m_sections.push_back(m_data.size());
            m_pathLengths.push_back(m_path.size());
            if (!m_path.empty())
                m_path += '/';
            m_path.append(tag, 4u);
            m_sectionPaths.insert(m_path);
            u64(0u);
        }

        void endSection()
        {
            if (m_sections.empty())
                throw StateError("snapshot writer: endSection without beginSection");
            const size_t lengthAt = m_sections.back();
            m_sections.pop_back();
            m_path.resize(m_pathLengths.back());
            m_pathLengths.pop_back();
            const uint64_t length = static_cast<uint64_t>(m_data.size() - lengthAt - sizeof(uint64_t));
            std::memcpy(m_data.data() + lengthAt, &length, sizeof(length));
        }

        [[nodiscard]] const std::vector<uint8_t> &data() const noexcept { return m_data; }
        [[nodiscard]] std::vector<uint8_t> &data() noexcept { return m_data; }
        void reserve(size_t bytes) { m_data.reserve(bytes); }

        // Raw types written so far, each with the section paths ("STUB/sRPC") that hold it.
        [[nodiscard]] const std::map<std::string, std::set<std::string>> &typeUse() const noexcept { return m_typeUse; }
        // Every section path written so far.
        [[nodiscard]] const std::set<std::string> &sectionPaths() const noexcept { return m_sectionPaths; }

    private:
        template <typename Key>
        void writeKey(const Key &key)
        {
            if constexpr (std::is_same_v<Key, std::string>)
                str(key);
            else
                pod(key);
        }

        std::vector<uint8_t> m_data;
        std::vector<size_t> m_sections;
        std::vector<size_t> m_pathLengths;
        std::string m_path;
        std::map<std::string, std::set<std::string>> m_typeUse;
        std::set<std::string> m_sectionPaths;
    };

    class StateReader
    {
    public:
        StateReader(const uint8_t *data, size_t size) : m_data(data), m_size(size) {}

        void bytes(void *destination, size_t size)
        {
            if (size > m_size - m_pos)
                throw StateError("snapshot is truncated" + context());
            if (size != 0u)
                std::memcpy(destination, m_data + m_pos, size);
            m_pos += size;
        }

        // A view of the next `size` bytes, consumed.
        const uint8_t *take(size_t size)
        {
            if (size > m_size - m_pos)
                throw StateError("snapshot is truncated" + context());
            const uint8_t *view = m_data + m_pos;
            m_pos += size;
            return view;
        }

        template <typename T>
        void pod(T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "pod() needs a trivially copyable type");
            if constexpr (kRecordedType<T>)
                (void)TypeRecord<T>::registered;
            bytes(&value, sizeof(T));
        }

        template <typename T>
        T pod()
        {
            T value{};
            pod(value);
            return value;
        }

        uint8_t u8() { return pod<uint8_t>(); }
        uint16_t u16() { return pod<uint16_t>(); }
        uint32_t u32() { return pod<uint32_t>(); }
        uint64_t u64() { return pod<uint64_t>(); }
        int32_t i32() { return pod<int32_t>(); }
        int64_t i64() { return pod<int64_t>(); }
        bool boolean() { return u8() != 0u; }

        size_t size(size_t limit = 1ull << 32)
        {
            const uint64_t value = u64();
            if (value > limit || value > m_size)
                throw StateError("snapshot holds an impossible count" + context());
            return static_cast<size_t>(value);
        }

        std::string str()
        {
            const size_t length = size();
            std::string value(length, '\0');
            bytes(value.data(), length);
            return value;
        }

        template <typename T>
        void podVector(std::vector<T> &values)
        {
            static_assert(std::is_trivially_copyable_v<T>, "podVector() needs a trivially copyable type");
            const size_t count = size();
            if (count > (m_size - m_pos) / std::max<size_t>(1u, sizeof(T)))
                throw StateError("snapshot is truncated" + context());
            values.resize(count);
            bytes(values.data(), count * sizeof(T));
        }

        template <typename Container>
        void podSequence(Container &values)
        {
            values.clear();
            const size_t count = size();
            for (size_t i = 0; i < count; ++i)
                values.push_back(pod<typename Container::value_type>());
        }

        void boolVector(std::vector<bool> &values)
        {
            const size_t count = size();
            values.assign(count, false);
            for (size_t i = 0; i < count; ++i)
                values[i] = boolean();
        }

        template <typename Map, typename ReadValue>
        void sortedMap(Map &map, ReadValue &&readValue)
        {
            using Key = typename Map::key_type;
            map.clear();
            const size_t count = size();
            for (size_t i = 0; i < count; ++i)
            {
                Key key = readKey<Key>();
                readValue(map[key]);
            }
        }

        template <typename Map>
        void podMap(Map &map)
        {
            sortedMap(map, [this](auto &value)
                      { pod(value); });
        }

        template <typename Set>
        void sortedSet(Set &set)
        {
            using Key = typename Set::key_type;
            set.clear();
            const size_t count = size();
            for (size_t i = 0; i < count; ++i)
                set.insert(readKey<Key>());
        }

        void beginSection(const char (&tag)[5])
        {
            char found[4]{};
            bytes(found, 4u);
            if (std::memcmp(found, tag, 4u) != 0)
            {
                throw StateError(std::string("snapshot section '") + std::string(tag, 4u) + "' expected, found '" +
                                 std::string(found, 4u) + "'" + context());
            }
            const uint64_t length = u64();
            if (length > m_size - m_pos)
                throw StateError(std::string("snapshot section '") + std::string(tag, 4u) + "' is truncated");
            m_sections.push_back({std::string(tag, 4u), m_pos + static_cast<size_t>(length)});
        }

        void endSection()
        {
            if (m_sections.empty())
                throw StateError("snapshot reader: endSection without beginSection");
            const Section section = m_sections.back();
            m_sections.pop_back();
            if (m_pos != section.end)
            {
                throw StateError("snapshot section '" + section.tag + "' read " +
                                 std::to_string(static_cast<int64_t>(m_pos) - static_cast<int64_t>(section.end)) +
                                 " bytes past its end (negative: left unread): its layout changed without a "
                                 "section version bump");
            }
        }

        [[nodiscard]] bool atEnd() const noexcept { return m_pos == m_size; }
        [[nodiscard]] size_t position() const noexcept { return m_pos; }

    private:
        struct Section
        {
            std::string tag;
            size_t end = 0u;
        };

        template <typename Key>
        Key readKey()
        {
            if constexpr (std::is_same_v<Key, std::string>)
                return str();
            else
                return pod<Key>();
        }

        std::string context() const
        {
            return m_sections.empty() ? std::string() : " (in section '" + m_sections.back().tag + "')";
        }

        const uint8_t *m_data = nullptr;
        size_t m_size = 0u;
        size_t m_pos = 0u;
        std::vector<Section> m_sections;
    };
}
