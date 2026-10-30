// search-benchmark-game driver for lucene_core.
//
// Speaks the same line protocol as the reference drivers, so the three engines
// can be compared on the same corpus, index and queries:
//
//     clucene_sbg <index|serve|verify|info> <index-dir>
//
// `index`  reads the corpus (one JSON object per line: `id`, `text`,
//          `sort_field`) from stdin and builds a single-segment index.
// `serve`  answers `COMMAND<TAB>QUERY` lines with the timed response.
// `verify` answers the same lines with the full JSON result, which is what
//          correctness comparison uses.
//
// Build with -DLUCENE_CORE_BUILD_BENCHMARKS=ON.

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

import lucene_core;

using namespace lucene;

namespace {

// ---------------------------------------------------------------- corpus JSON

/// Appends the UTF-8 encoding of `code_point`.
void append_utf8(std::string &out, uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

uint32_t parse_hex4(std::string_view text, size_t at) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = text[at + i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(c - 'A' + 10);
        }
        value = (value << 4) | digit;
    }
    return value;
}

/// Reads the JSON string starting at `pos` (which must point at the opening
/// quote) into `out` and returns the position just past the closing quote.
size_t read_json_string(std::string_view text, size_t pos, std::string &out) {
    ++pos;  // opening quote
    while (pos < text.size()) {
        const char c = text[pos];
        if (c == '"') {
            return pos + 1;
        }
        if (c != '\\') {
            out.push_back(c);
            ++pos;
            continue;
        }
        ++pos;
        if (pos >= text.size()) {
            break;
        }
        switch (text[pos]) {
            case '"': out.push_back('"'); ++pos; break;
            case '\\': out.push_back('\\'); ++pos; break;
            case '/': out.push_back('/'); ++pos; break;
            case 'b': out.push_back('\b'); ++pos; break;
            case 'f': out.push_back('\f'); ++pos; break;
            case 'n': out.push_back('\n'); ++pos; break;
            case 'r': out.push_back('\r'); ++pos; break;
            case 't': out.push_back('\t'); ++pos; break;
            case 'u': {
                if (pos + 4 >= text.size()) {
                    return text.size();
                }
                const uint32_t unit = parse_hex4(text, pos + 1);
                pos += 5;
                if (unit >= 0xD800 && unit <= 0xDBFF && pos + 1 < text.size() && text[pos] == '\\' &&
                    text[pos + 1] == 'u' && pos + 5 < text.size()) {
                    const uint32_t low = parse_hex4(text, pos + 2);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        pos += 6;
                        append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                        break;
                    }
                }
                append_utf8(out, unit);
                break;
            }
            default: out.push_back(text[pos]); ++pos; break;
        }
    }
    return pos;
}

/// Position of the value of `key`, or `npos`.  Accepts any whitespace around
/// the colon, which `json.dumps` may or may not emit.
size_t json_value_position(std::string_view line, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const size_t at = line.find(needle);
    if (at == std::string_view::npos) {
        return std::string_view::npos;
    }
    size_t pos = at + needle.size();
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != ':') {
        return std::string_view::npos;
    }
    ++pos;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

bool json_string_field(std::string_view line, std::string_view key, std::string &out) {
    const size_t pos = json_value_position(line, key);
    if (pos == std::string_view::npos || pos >= line.size() || line[pos] != '"') {
        return false;
    }
    out.clear();
    read_json_string(line, pos, out);
    return true;
}

bool json_int_field(std::string_view line, std::string_view key, int64_t &out) {
    size_t pos = json_value_position(line, key);
    if (pos == std::string_view::npos) {
        return false;
    }
    bool negative = false;
    if (pos < line.size() && line[pos] == '-') {
        negative = true;
        ++pos;
    }
    int64_t value = 0;
    bool any = false;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        value = value * 10 + (line[pos] - '0');
        any = true;
        ++pos;
    }
    if (!any) {
        return false;
    }
    out = negative ? -value : value;
    return true;
}

// ------------------------------------------------------------------ utilities

[[noreturn]] void die(const char *what, const LuceneError &error) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, error.to_string().c_str());
    std::exit(1);
}

void die_if(const char *what, const VoidResult &result) {
    if (!result) {
        die(what, result.error());
    }
}

/// The analyzer the benchmark pins: `StandardAnalyzer` with an empty stop set,
/// which is what the Java driver's `CharArraySet.EMPTY_SET` means.  The port's
/// default constructor installs the English stop words, so it must not be used
/// here: it would drop `the`, `and`, ... and change every score.
StandardAnalyzer make_analyzer() { return StandardAnalyzer(std::set<std::string, std::less<>>{}); }

// --------------------------------------------------------------------- modes

int run_index(const std::string &path) {
    auto directory = MMapDirectory::open(path);
    if (!directory) {
        die("open directory", directory.error());
    }
    auto analyzer = make_analyzer();

    IndexWriterConfig config(analyzer);
    config.set_open_mode(OpenMode::Create);
    config.set_ram_buffer_size_mb(1024.0);
    // Both drivers must agree on the physical layout for the byte comparison to
    // be meaningful; `SBG_NO_COMPOUND=1` writes plain `.tim/.doc/...` files.
    if (const char *no_compound = std::getenv("SBG_NO_COMPOUND"); no_compound != nullptr && std::strcmp(no_compound, "0") != 0) {
        config.set_use_compound_file(false);
    }
    auto writer = IndexWriter::open(**directory, config);
    if (!writer) {
        die("open writer", writer.error());
    }

    std::string line;
    size_t docs = 0;
    std::string id;
    std::string text;
    int64_t sort_field = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        if (!json_string_field(line, "id", id)) {
            std::fprintf(stderr, "document without an id at %zu\n", docs);
            return 1;
        }
        if (!json_string_field(line, "text", text)) {
            text.clear();
        }
        if (!json_int_field(line, "sort_field", sort_field)) {
            sort_field = 0;
        }

        Document document;
        document.add(std::make_unique<Field>(StoredField::from_string("id", id)));
        document.add(std::make_unique<Field>(std::move(TextField::create("text", text, false).value())));
        document.add(std::make_unique<Field>(std::move(NumericDocValuesField::create("sort_field", sort_field).value())));
        die_if("add document", (*writer)->add_document(document));
        ++docs;
        if (docs % 100000 == 0) {
            std::fprintf(stderr, "indexed %zu documents\n", docs);
        }
    }

    die_if("force merge", (*writer)->force_merge(1));
    die_if("close writer", (*writer)->close());
    std::fprintf(stderr, "indexed %zu documents into %s\n", docs, path.c_str());
    return 0;
}

/// What one `COMMAND<TAB>QUERY` line asks for: `k == 0` means count only,
/// `exact` means the total hit count must not become a lower bound.
struct Mode {
    int32_t k{0};
    bool exact{true};
    bool known{true};
};

Mode parse_mode(std::string_view command) {
    if (command == "COUNT" || command == "UNOPTIMIZED_COUNT") {
        return Mode{0, true, true};
    }
    if (command == "TOP_10") {
        return Mode{10, false, true};
    }
    if (command == "TOP_100") {
        return Mode{100, false, true};
    }
    if (command == "TOP_1000") {
        return Mode{1000, false, true};
    }
    if (command == "TOP_10_COUNT") {
        return Mode{10, true, true};
    }
    if (command == "TOP_100_COUNT") {
        return Mode{100, true, true};
    }
    if (command == "TOP_1000_COUNT") {
        return Mode{1000, true, true};
    }
    return Mode{0, true, false};
}

const char *relation_name(TotalHits::Relation relation) {
    return relation == TotalHits::Relation::EqualTo ? "EqualTo" : "GreaterThanOrEqualTo";
}

void write_escaped(std::string &out, std::string_view value) {
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
}

int run_search(const std::string &path, bool verify) {
    auto directory = MMapDirectory::open(path);
    if (!directory) {
        die("open directory", directory.error());
    }
    auto reader = DirectoryReader::open(**directory);
    if (!reader) {
        die("open reader", reader.error());
    }
    auto similarity = BM25Similarity::with_k1_b(1.2F, 0.75F);
    if (!similarity) {
        die("bm25", similarity.error());
    }
    auto shared_similarity = std::make_shared<BM25Similarity>(std::move(similarity.value()));
    IndexSearcher searcher(*reader, shared_similarity);
    // The other drivers do the same: a cached query would time the cache instead
    // of the engine, and the cached (document-set) path bypasses the bulk scorers
    // that do the skipping.
    searcher.set_query_cache(nullptr);
    auto analyzer = make_analyzer();
    QueryParser parser("text", analyzer);

    std::fprintf(stderr, "INDEX max_doc=%d num_docs=%d deletions=%d\n",
                 (*reader)->max_doc().value(), (*reader)->num_docs().value(), (*reader)->has_deletions().value() ? 1 : 0);

    std::unique_ptr<StoredFields> stored;
    if (verify) {
        auto fields = (*reader)->stored_fields();
        if (!fields) {
            die("stored fields", fields.error());
        }
        stored = std::move(fields.value());
    }

    std::string line;
    std::string response;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            std::fprintf(stderr, "expected COMMAND<TAB>QUERY\n");
            return 1;
        }
        const std::string_view command(line.data(), tab);
        const std::string_view text(line.data() + tab + 1, line.size() - tab - 1);
        const Mode mode = parse_mode(command);
        if (!mode.known) {
            std::fputs("UNSUPPORTED\n", stdout);
            std::fflush(stdout);
            continue;
        }

        auto query = parser.parse(text);
        if (!query) {
            die("parse query", query.error());
        }
        if (*query == nullptr) {
            // Every clause was filtered out: an empty query matches nothing, like
            // the reference implementation.
            if (mode.k == 0) {
                std::fputs(verify ? "{\"count\":0}\n" : "0\n", stdout);
            } else {
                std::fputs(verify ? "{\"count\":0,\"relation\":\"EqualTo\",\"docs\":[]}\n" : "0\n", stdout);
            }
            std::fflush(stdout);
            continue;
        }

        if (mode.k == 0) {
            auto count = searcher.count(**query);
            if (!count) {
                die("count", count.error());
            }
            if (verify) {
                response = "{\"count\":" + std::to_string(count.value()) + "}";
            } else {
                response = std::to_string(count.value());
            }
        } else {
            const size_t threshold = mode.exact ? static_cast<size_t>(INT32_MAX) : static_cast<size_t>(mode.k);
            auto manager = TopScoreDocCollectorManager::create(static_cast<size_t>(mode.k), threshold);
            if (!manager) {
                die("collector manager", manager.error());
            }
            auto hits = searcher.search_with_collector_manager(**query, **manager);
            if (!hits) {
                die("search", hits.error());
            }
            if (!verify) {
                response = mode.exact ? std::to_string(hits.value().total_hits.value) : "1";
            } else {
                response = "{\"count\":" + std::to_string(hits.value().total_hits.value) + ",\"relation\":\"" +
                        relation_name(hits.value().total_hits.relation) + "\",\"docs\":[";
                bool first = true;
                for (const ScoreDoc &hit : hits.value().score_docs) {
                    auto document = stored->document(hit.doc);
                    if (!document) {
                        die("stored document", document.error());
                    }
                    auto id = document.value().get("id");
                    if (!id || !id.value().has_value()) {
                        std::fprintf(stderr, "missing stored document id\n");
                        return 1;
                    }
                    if (!first) {
                        response.push_back(',');
                    }
                    first = false;
                    response += "{\"id\":\"";
                    write_escaped(response, id.value().value());
                    response += "\",\"doc\":" + std::to_string(hit.doc) + ",\"score_bits\":" +
                            std::to_string(std::bit_cast<uint32_t>(hit.score)) + "}";
                }
                response += "]}";
            }
        }
        response.push_back('\n');
        std::fwrite(response.data(), 1, response.size(), stdout);
        std::fflush(stdout);
    }
    return 0;
}

int run_info(const std::string &path) {
    auto directory = MMapDirectory::open(path);
    if (!directory) {
        die("open directory", directory.error());
    }
    auto reader = DirectoryReader::open(**directory);
    if (!reader) {
        die("open reader", reader.error());
    }
    std::printf("{\"max_doc\":%d,\"num_docs\":%d,\"deletions\":%s}\n",
                (*reader)->max_doc().value(),
                (*reader)->num_docs().value(),
                (*reader)->has_deletions().value() ? "true" : "false");
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    std::ios::sync_with_stdio(false);
    if (argc != 3) {
        std::fprintf(stderr, "usage: clucene_sbg <index|serve|verify|info> <index-dir>\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::string path = argv[2];
    if (mode == "index") {
        return run_index(path);
    }
    if (mode == "serve") {
        return run_search(path, false);
    }
    if (mode == "verify") {
        return run_search(path, true);
    }
    if (mode == "info") {
        return run_info(path);
    }
    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 2;
}
