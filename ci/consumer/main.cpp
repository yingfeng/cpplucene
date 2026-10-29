// Smoke test for the installed package: index three documents, commit, reopen the
// index through a DirectoryReader, run a TermQuery and read a stored field back.
//
// A failure here means the install tree is not usable by a downstream project,
// which no in-tree unit test can detect.

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

import lucene_core;

using namespace lucene;

namespace {

FieldType text_type() {
    FieldType type;
    (void)type.set_index_options(IndexOptions::DocsAndFreqsAndPositions);
    (void)type.set_stored(true);
    return type;
}

void add_text(Document &doc, std::string_view name, std::string value) {
    doc.add(std::make_unique<Field>(std::string(name), FieldValue{std::move(value)}, text_type()));
}

Document make_doc(std::string id, std::string title) {
    Document doc;
    add_text(doc, "id", std::move(id));
    add_text(doc, "title", std::move(title));
    return doc;
}

} // namespace

int main() {
    ByteBuffersDirectory dir;
    // The writer borrows the analyzer, so it has to outlive it.
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);

    {
        auto writer = IndexWriter::open(dir, config);
        if (!writer.has_value()) {
            std::cerr << "open failed: " << writer.error().to_string() << '\n';
            return 1;
        }
        for (const auto &[id, title] : {std::pair<std::string, std::string>{"a", "hello world"},
                                        std::pair<std::string, std::string>{"b", "hello lucene"},
                                        std::pair<std::string, std::string>{"c", "world"}}) {
            if (!writer.value()->add_document(make_doc(id, title)).has_value()) {
                std::cerr << "add_document failed\n";
                return 1;
            }
        }
        if (!writer.value()->commit().has_value()) {
            std::cerr << "commit failed\n";
            return 1;
        }
    }

    auto reader = DirectoryReader::open(dir);
    if (!reader.has_value()) {
        std::cerr << "open reader failed: " << reader.error().to_string() << '\n';
        return 1;
    }
    if (reader.value()->num_docs().value() != 3) {
        std::cerr << "expected 3 documents\n";
        return 1;
    }

    IndexSearcher searcher(reader.value());
    TermQuery query(Term("title", BytesRef::from_string("hello")));
    auto top = searcher.search(query, 10);
    if (!top.has_value()) {
        std::cerr << "search failed: " << top.error().to_string() << '\n';
        return 1;
    }
    const TopDocs &hits = top.value();
    if (hits.score_docs.size() != 2) {
        std::cerr << "expected 2 hits, got " << hits.score_docs.size() << '\n';
        return 1;
    }

    auto leaves = reader.value()->leaves();
    if (!leaves.has_value() || leaves.value().empty()) {
        std::cerr << "no leaves\n";
        return 1;
    }
    auto stored = leaves.value()[0].reader()->stored_fields();
    if (!stored.has_value()) {
        std::cerr << "stored_fields failed\n";
        return 1;
    }
    auto doc = stored.value()->document(hits.score_docs.front().doc);
    if (!doc.has_value() || !doc.value().get("id").has_value()) {
        std::cerr << "stored field round trip failed\n";
        return 1;
    }

    std::cout << "ok: num_docs=" << reader.value()->num_docs().value() << " hits=" << hits.score_docs.size()
              << " stored_id=" << doc.value().get("id").value().value() << '\n';
    return 0;
}
