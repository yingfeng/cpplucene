// search-benchmark-game driver for Apache Lucene (the reference implementation).
//
// Speaks exactly the same line protocol as `clucene_sbg`, so clucene, Lucene and
// any other driver can be compared on the same corpus, index and queries:
//
//     java_sbg <index|serve|verify|info> <index-dir>
//
// `index`  reads the corpus (one JSON object per line: `id`, `text`,
//          `sort_field`) from stdin and builds a single-segment index.
// `serve`  answers `COMMAND<TAB>QUERY` lines with the timed response.
// `verify` answers the same lines with the full JSON result.
// `info`   prints the index geometry.
//
// Schema, analyzer and similarity mirror `clucene_sbg`, which mirrors the
// published search-benchmark-game drivers: a stored `id`, a tokenized `text`
// (StandardAnalyzer with an empty stop set) and a `sort_field` doc value.

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Paths;

import org.apache.lucene.analysis.Analyzer;
import org.apache.lucene.analysis.CharArraySet;
import org.apache.lucene.analysis.standard.StandardAnalyzer;
import org.apache.lucene.document.Document;
import org.apache.lucene.document.Field;
import org.apache.lucene.document.NumericDocValuesField;
import org.apache.lucene.document.StoredField;
import org.apache.lucene.document.TextField;
import org.apache.lucene.index.DirectoryReader;
import org.apache.lucene.index.IndexWriter;
import org.apache.lucene.index.IndexWriterConfig;
import org.apache.lucene.index.StoredFields;
import org.apache.lucene.queryparser.classic.QueryParser;
import org.apache.lucene.search.similarities.BM25Similarity;
import org.apache.lucene.search.IndexSearcher;
import org.apache.lucene.search.Query;
import org.apache.lucene.search.ScoreDoc;
import org.apache.lucene.search.TopDocs;
import org.apache.lucene.search.TopScoreDocCollectorManager;
import org.apache.lucene.search.TotalHits;
import org.apache.lucene.store.Directory;
import org.apache.lucene.store.MMapDirectory;

public final class java_sbg {

    private static final String FIELD_TEXT = "text";
    private static final String FIELD_ID = "id";

    private static Analyzer makeAnalyzer() {
        // Empty stop set: the port's `StandardAnalyzer` default drops English stop
        // words, so both sides must be told to keep every token.
        return new StandardAnalyzer(CharArraySet.EMPTY_SET);
    }

    // -------------------------------------------------------------- JSON input

    /** Minimal JSON string field reader for the corpus lines. */
    private static String jsonString(String line, String key) {
        String needle = "\"" + key + "\"";
        int at = line.indexOf(needle);
        if (at < 0) {
            return null;
        }
        int pos = at + needle.length();
        while (pos < line.length() && (line.charAt(pos) == ' ' || line.charAt(pos) == '\t')) {
            pos++;
        }
        if (pos >= line.length() || line.charAt(pos) != ':') {
            return null;
        }
        pos++;
        while (pos < line.length() && (line.charAt(pos) == ' ' || line.charAt(pos) == '\t')) {
            pos++;
        }
        if (pos >= line.length() || line.charAt(pos) != '"') {
            return null;
        }
        pos++;
        StringBuilder out = new StringBuilder(line.length() / 2);
        while (pos < line.length()) {
            char c = line.charAt(pos);
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                out.append(c);
                pos++;
                continue;
            }
            pos++;
            if (pos >= line.length()) {
                break;
            }
            char e = line.charAt(pos);
            switch (e) {
                case '"' -> { out.append('"'); pos++; }
                case '\\' -> { out.append('\\'); pos++; }
                case '/' -> { out.append('/'); pos++; }
                case 'b' -> { out.append('\b'); pos++; }
                case 'f' -> { out.append('\f'); pos++; }
                case 'n' -> { out.append('\n'); pos++; }
                case 'r' -> { out.append('\r'); pos++; }
                case 't' -> { out.append('\t'); pos++; }
                case 'u' -> {
                    if (pos + 4 < line.length()) {
                        out.append((char) Integer.parseInt(line.substring(pos + 1, pos + 5), 16));
                        pos += 5;
                    } else {
                        pos = line.length();
                    }
                }
                default -> { out.append(e); pos++; }
            }
        }
        return out.toString();
    }

    private static long jsonLong(String line, String key, long fallback) {
        String needle = "\"" + key + "\"";
        int at = line.indexOf(needle);
        if (at < 0) {
            return fallback;
        }
        int pos = at + needle.length();
        while (pos < line.length() && (line.charAt(pos) == ' ' || line.charAt(pos) == '\t')) {
            pos++;
        }
        if (pos >= line.length() || line.charAt(pos) != ':') {
            return fallback;
        }
        pos++;
        while (pos < line.length() && (line.charAt(pos) == ' ' || line.charAt(pos) == '\t')) {
            pos++;
        }
        int start = pos;
        if (pos < line.length() && (line.charAt(pos) == '-' || line.charAt(pos) == '+')) {
            pos++;
        }
        while (pos < line.length() && Character.isDigit(line.charAt(pos))) {
            pos++;
        }
        if (pos == start) {
            return fallback;
        }
        return Long.parseLong(line.substring(start, pos));
    }

    // ------------------------------------------------------------------ modes

    private record Mode(int k, boolean exact, boolean known) {}

    private static Mode parseMode(String command) {
        return switch (command) {
            case "COUNT", "UNOPTIMIZED_COUNT" -> new Mode(0, true, true);
            case "TOP_10" -> new Mode(10, false, true);
            case "TOP_100" -> new Mode(100, false, true);
            case "TOP_1000" -> new Mode(1000, false, true);
            case "TOP_10_COUNT" -> new Mode(10, true, true);
            case "TOP_100_COUNT" -> new Mode(100, true, true);
            case "TOP_1000_COUNT" -> new Mode(1000, true, true);
            default -> new Mode(0, true, false);
        };
    }

    private static String relationName(TotalHits.Relation relation) {
        return relation == TotalHits.Relation.EQUAL_TO ? "EqualTo" : "GreaterThanOrEqualTo";
    }

    private static String escape(String value) {
        StringBuilder out = new StringBuilder(value.length() + 8);
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (c == '"' || c == '\\') {
                out.append('\\');
            }
            out.append(c);
        }
        return out.toString();
    }

    // ------------------------------------------------------------------- index

    private static int runIndex(String path) throws IOException {
        Directory directory = new MMapDirectory(Paths.get(path));
        // `SBG_NO_COMPOUND=1` writes plain `.tim/.doc/...` files, like the other
        // drivers, so the physical layout comparison stays meaningful.
        String noCompound = System.getenv("SBG_NO_COMPOUND");
        boolean useCompound = noCompound == null || noCompound.equals("0");
        try (IndexWriter writer = new IndexWriter(directory, new IndexWriterConfig(makeAnalyzer())
                .setOpenMode(IndexWriterConfig.OpenMode.CREATE)
                .setRAMBufferSizeMB(1024.0)
                .setUseCompoundFile(useCompound))) {
            BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
            long docs = 0;
            String line;
            while ((line = in.readLine()) != null) {
                if (line.isEmpty()) {
                    continue;
                }
                String id = jsonString(line, "id");
                if (id == null) {
                    System.err.printf("document without an id at %d%n", docs);
                    return 1;
                }
                String text = jsonString(line, "text");
                if (text == null) {
                    text = "";
                }
                long sortField = jsonLong(line, "sort_field", 0L);

                Document document = new Document();
                document.add(new StoredField(FIELD_ID, id));
                document.add(new TextField(FIELD_TEXT, text, Field.Store.NO));
                document.add(new NumericDocValuesField("sort_field", sortField));
                writer.addDocument(document);
                docs++;
                if (docs % 100000 == 0) {
                    System.err.printf("indexed %d documents%n", docs);
                }
            }
            writer.forceMerge(1);
            System.err.printf("indexed %d documents into %s%n", docs, path);
        } finally {
            directory.close();
        }
        return 0;
    }

    // ------------------------------------------------------------------ search

    private static int runSearch(String path, boolean verify) throws Exception {
        Directory directory = new MMapDirectory(Paths.get(path));
        DirectoryReader reader = DirectoryReader.open(directory);
        IndexSearcher searcher = new IndexSearcher(reader);
        searcher.setSimilarity(new BM25Similarity(1.2f, 0.75f));
        // The benchmark replays the same queries many times; the default query
        // cache would then measure the cache instead of the engine (the other
        // drivers have no cache either).
        searcher.setQueryCache(null);
        Analyzer analyzer = makeAnalyzer();
        QueryParser parser = new QueryParser(FIELD_TEXT, analyzer);

        System.err.printf("INDEX max_doc=%d num_docs=%d deletions=%d%n",
                reader.maxDoc(), reader.numDocs(), reader.hasDeletions() ? 1 : 0);

        StoredFields stored = verify ? reader.storedFields() : null;
        PrintStream out = new PrintStream(new java.io.BufferedOutputStream(System.out), false, StandardCharsets.UTF_8);
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));

        String line;
        while ((line = in.readLine()) != null) {
            if (line.isEmpty()) {
                continue;
            }
            int tab = line.indexOf('\t');
            if (tab < 0) {
                System.err.println("expected COMMAND<TAB>QUERY");
                return 1;
            }
            String command = line.substring(0, tab);
            String text = line.substring(tab + 1);
            Mode mode = parseMode(command);
            if (!mode.known()) {
                out.print("UNSUPPORTED\n");
                out.flush();
                continue;
            }

            Query query;
            try {
                query = parser.parse(text);
            } catch (org.apache.lucene.queryparser.classic.ParseException e) {
                // An empty text parses to nothing, which matches nothing.
                query = null;
            }

            String response;
            if (query == null) {
                response = mode.k() == 0
                        ? (verify ? "{\"count\":0}" : "0")
                        : (verify ? "{\"count\":0,\"relation\":\"EqualTo\",\"docs\":[]}" : "0");
            } else if (mode.k() == 0) {
                int count = searcher.count(query);
                response = verify ? "{\"count\":" + count + "}" : Integer.toString(count);
            } else {
                int threshold = mode.exact() ? Integer.MAX_VALUE : mode.k();
                TopDocs hits = searcher.search(query, new TopScoreDocCollectorManager(mode.k(), threshold));
                if (!verify) {
                    response = mode.exact() ? Long.toString(hits.totalHits.value()) : "1";
                } else {
                    StringBuilder sb = new StringBuilder(256);
                    sb.append("{\"count\":").append(hits.totalHits.value())
                            .append(",\"relation\":\"").append(relationName(hits.totalHits.relation()))
                            .append("\",\"docs\":[");
                    boolean first = true;
                    for (ScoreDoc hit : hits.scoreDocs) {
                        Document document = stored.document(hit.doc);
                        String id = document.get(FIELD_ID);
                        if (!first) {
                            sb.append(',');
                        }
                        first = false;
                        sb.append("{\"id\":\"").append(escape(id))
                                .append("\",\"doc\":").append(hit.doc)
                                .append(",\"score_bits\":").append(Float.floatToRawIntBits(hit.score))
                                .append('}');
                    }
                    sb.append("]}");
                    response = sb.toString();
                }
            }
            out.print(response);
            out.print('\n');
            out.flush();
        }
        out.flush();
        reader.close();
        directory.close();
        return 0;
    }

    private static int runInfo(String path) throws IOException {
        Directory directory = new MMapDirectory(Paths.get(path));
        try (DirectoryReader reader = DirectoryReader.open(directory)) {
            System.out.printf("{\"max_doc\":%d,\"num_docs\":%d,\"deletions\":%s}%n",
                    reader.maxDoc(), reader.numDocs(), reader.hasDeletions() ? "true" : "false");
        } finally {
            directory.close();
        }
        return 0;
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 2) {
            System.err.println("usage: java_sbg <index|serve|verify|info> <index-dir>");
            return;
        }
        String mode = args[0];
        String path = args[1];
        switch (mode) {
            case "index" -> {
                if (runIndex(path) != 0) {
                    System.exit(1);
                }
            }
            case "serve" -> {
                if (runSearch(path, false) != 0) {
                    System.exit(1);
                }
            }
            case "verify" -> {
                if (runSearch(path, true) != 0) {
                    System.exit(1);
                }
            }
            case "info" -> {
                if (runInfo(path) != 0) {
                    System.exit(1);
                }
            }
            default -> {
                System.err.println("usage: java_sbg <index|serve|verify|info> <index-dir>");
                System.exit(1);
            }
        }
    }
}
