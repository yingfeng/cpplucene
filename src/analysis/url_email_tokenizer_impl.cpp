/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

module lucene_core:url_email_tokenizer.impl;

import :url_email_tokenizer;

import :attributes;
import :standard_tokenizer;
import :tokenizer;
import :types;

import std;

namespace lucene {

namespace {

// ---------------------------------------------------------------------------
// Top level domains
//
// Generated from the same IANA list the reference implementation's
// `ASCIITLD.jflex` macros are generated from (`ASCIITLD`, plus the two
// "prefix" sets that exist only because JFlex cannot share prefixes).  The
// array is sorted, so lookup is a binary search on the lower-cased label.
// ---------------------------------------------------------------------------

/// Sorted, lower case; see `is_tld`.
constexpr std::string_view kTopLevelDomains[] = {
    "aaa", "aarp", "abb", "abbott", "abbvie", "abc",
    "able", "abogado", "abudhabi", "ac", "academy", "accenture",
    "accountant", "accountants", "aco", "actor", "ad", "ads",
    "adult", "ae", "aeg", "aero", "aetna", "af",
    "afl", "africa", "ag", "agakhan", "agency", "ai",
    "aig", "airbus", "airforce", "airtel", "akdn", "al",
    "alibaba", "alipay", "allfinanz", "allstate", "ally", "alsace",
    "alstom", "am", "amazon", "americanexpress", "americanfamily", "amex",
    "amfam", "amica", "amsterdam", "analytics", "android", "anquan",
    "anz", "ao", "aol", "apartments", "app", "apple",
    "aq", "aquarelle", "ar", "arab", "aramco", "archi",
    "army", "arpa", "art", "arte", "as", "asda",
    "asia", "associates", "at", "athleta", "attorney", "au",
    "auction", "audi", "audible", "audio", "auspost", "author",
    "auto", "autos", "aw", "aws", "ax", "axa",
    "az", "azure", "ba", "baby", "baidu", "banamex",
    "band", "bank", "bar", "barcelona", "barclaycard", "barclays",
    "barefoot", "bargains", "baseball", "basketball", "bauhaus", "bayern",
    "bb", "bbc", "bbt", "bbva", "bcg", "bcn",
    "bd", "be", "beats", "beauty", "beer", "bentley",
    "berlin", "best", "bestbuy", "bet", "bf", "bg",
    "bh", "bharti", "bi", "bible", "bid", "bike",
    "bing", "bingo", "bio", "biz", "bj", "black",
    "blackfriday", "blockbuster", "blog", "bloomberg", "blue", "bm",
    "bms", "bmw", "bn", "bnpparibas", "bo", "boats",
    "boehringer", "bofa", "bom", "bond", "boo", "book",
    "booking", "bosch", "bostik", "boston", "bot", "boutique",
    "box", "br", "bradesco", "bridgestone", "broadway", "broker",
    "brother", "brussels", "bs", "bt", "build", "builders",
    "business", "buy", "buzz", "bv", "bw", "by",
    "bz", "bzh", "ca", "cab", "cafe", "cal",
    "call", "calvinklein", "cam", "camera", "camp", "canon",
    "capetown", "capital", "capitalone", "car", "caravan", "cards",
    "care", "career", "careers", "cars", "casa", "case",
    "cash", "casino", "cat", "catering", "catholic", "cba",
    "cbn", "cbre", "cc", "cd", "center", "ceo",
    "cern", "cf", "cfa", "cfd", "cg", "ch",
    "chanel", "channel", "charity", "chase", "chat", "cheap",
    "chintai", "christmas", "chrome", "church", "ci", "cipriani",
    "circle", "cisco", "citadel", "citi", "citic", "city",
    "ck", "cl", "claims", "cleaning", "click", "clinic",
    "clinique", "clothing", "cloud", "club", "clubmed", "cm",
    "cn", "co", "coach", "codes", "coffee", "college",
    "cologne", "com", "commbank", "community", "company", "compare",
    "computer", "comsec", "condos", "construction", "consulting", "contact",
    "contractors", "cooking", "cool", "coop", "corsica", "country",
    "coupon", "coupons", "courses", "cpa", "cr", "credit",
    "creditcard", "creditunion", "cricket", "crown", "crs", "cruise",
    "cruises", "cu", "cuisinella", "cv", "cw", "cx",
    "cy", "cymru", "cyou", "cz", "dabur", "dad",
    "dance", "data", "date", "dating", "datsun", "day",
    "dclk", "dds", "de", "deal", "dealer", "deals",
    "degree", "delivery", "dell", "deloitte", "delta", "democrat",
    "dental", "dentist", "desi", "design", "dev", "dhl",
    "diamonds", "diet", "digital", "direct", "directory", "discount",
    "discover", "dish", "diy", "dj", "dk", "dm",
    "dnp", "do", "docs", "doctor", "dog", "domains",
    "dot", "download", "drive", "dtv", "dubai", "dunlop",
    "dupont", "durban", "dvag", "dvr", "dz", "earth",
    "eat", "ec", "eco", "edeka", "edu", "education",
    "ee", "eg", "email", "emerck", "energy", "engineer",
    "engineering", "enterprises", "epson", "equipment", "er", "ericsson",
    "erni", "es", "esq", "estate", "et", "eu",
    "eurovision", "eus", "events", "exchange", "expert", "exposed",
    "express", "extraspace", "fage", "fail", "fairwinds", "faith",
    "family", "fan", "fans", "farm", "farmers", "fashion",
    "fast", "fedex", "feedback", "ferrari", "ferrero", "fi",
    "fidelity", "fido", "film", "final", "finance", "financial",
    "fire", "firestone", "firmdale", "fish", "fishing", "fit",
    "fitness", "fj", "fk", "flickr", "flights", "flir",
    "florist", "flowers", "fly", "fm", "fo", "foo",
    "food", "football", "ford", "forex", "forsale", "forum",
    "foundation", "fox", "fr", "free", "fresenius", "frl",
    "frogans", "frontier", "ftr", "fujitsu", "fun", "fund",
    "furniture", "futbol", "fyi", "ga", "gal", "gallery",
    "gallo", "gallup", "game", "games", "gap", "garden",
    "gay", "gb", "gbiz", "gd", "gdn", "ge",
    "gea", "gent", "genting", "george", "gf", "gg",
    "ggee", "gh", "gi", "gift", "gifts", "gives",
    "giving", "gl", "glass", "gle", "global", "globo",
    "gm", "gmail", "gmbh", "gmo", "gmx", "gn",
    "godaddy", "gold", "goldpoint", "golf", "goo", "goodyear",
    "goog", "google", "gop", "got", "gov", "gp",
    "gq", "gr", "grainger", "graphics", "gratis", "green",
    "gripe", "grocery", "group", "gs", "gt", "gu",
    "gucci", "guge", "guide", "guitars", "guru", "gw",
    "gy", "hair", "hamburg", "hangout", "haus", "hbo",
    "hdfc", "hdfcbank", "health", "healthcare", "help", "helsinki",
    "here", "hermes", "hiphop", "hisamitsu", "hitachi", "hiv",
    "hk", "hkt", "hm", "hn", "hockey", "holdings",
    "holiday", "homedepot", "homegoods", "homes", "homesense", "honda",
    "horse", "hospital", "host", "hosting", "hot", "hotels",
    "hotmail", "house", "how", "hr", "hsbc", "ht",
    "hu", "hughes", "hyatt", "hyundai", "ibm", "icbc",
    "ice", "icu", "id", "ie", "ieee", "ifm",
    "ikano", "il", "im", "imamat", "imdb", "immo",
    "immobilien", "in", "inc", "industries", "infiniti", "info",
    "ing", "ink", "institute", "insurance", "insure", "int",
    "international", "intuit", "investments", "io", "ipiranga", "iq",
    "ir", "irish", "is", "ismaili", "ist", "istanbul",
    "it", "itau", "itv", "jaguar", "java", "jcb",
    "je", "jeep", "jetzt", "jewelry", "jio", "jll",
    "jm", "jmp", "jnj", "jo", "jobs", "joburg",
    "jot", "joy", "jp", "jpmorgan", "jprs", "juegos",
    "juniper", "kaufen", "kddi", "ke", "kerryhotels", "kerrylogistics",
    "kerryproperties", "kfh", "kg", "kh", "ki", "kia",
    "kids", "kim", "kindle", "kitchen", "kiwi", "km",
    "kn", "koeln", "komatsu", "kosher", "kp", "kpmg",
    "kpn", "kr", "krd", "kred", "kuokgroup", "kw",
    "ky", "kyoto", "kz", "la", "lacaixa", "lamborghini",
    "lamer", "lancaster", "land", "landrover", "lanxess", "lasalle",
    "lat", "latino", "latrobe", "law", "lawyer", "lb",
    "lc", "lds", "lease", "leclerc", "lefrak", "legal",
    "lego", "lexus", "lgbt", "li", "lidl", "life",
    "lifeinsurance", "lifestyle", "lighting", "like", "lilly", "limited",
    "limo", "lincoln", "link", "lipsy", "live", "living",
    "lk", "llc", "llp", "loan", "loans", "locker",
    "locus", "lol", "london", "lotte", "lotto", "love",
    "lpl", "lplfinancial", "lr", "ls", "lt", "ltd",
    "ltda", "lu", "lundbeck", "luxe", "luxury", "lv",
    "ly", "ma", "madrid", "maif", "maison", "makeup",
    "man", "management", "mango", "map", "market", "marketing",
    "markets", "marriott", "marshalls", "mattel", "mba", "mc",
    "mckinsey", "md", "me", "med", "media", "meet",
    "melbourne", "meme", "memorial", "men", "menu", "merckmsd",
    "mg", "mh", "miami", "microsoft", "mil", "mini",
    "mint", "mit", "mitsubishi", "mk", "ml", "mlb",
    "mls", "mm", "mma", "mn", "mo", "mobi",
    "mobile", "moda", "moe", "moi", "mom", "monash",
    "money", "monster", "mormon", "mortgage", "moscow", "moto",
    "motorcycles", "mov", "movie", "mp", "mq", "mr",
    "ms", "msd", "mt", "mtn", "mtr", "mu",
    "museum", "music", "mv", "mw", "mx", "my",
    "mz", "na", "nab", "nagoya", "name", "natura",
    "navy", "nba", "nc", "ne", "nec", "net",
    "netbank", "netflix", "network", "neustar", "new", "news",
    "next", "nextdirect", "nexus", "nf", "nfl", "ng",
    "ngo", "nhk", "ni", "nico", "nike", "nikon",
    "ninja", "nissan", "nissay", "nl", "no", "nokia",
    "norton", "now", "nowruz", "nowtv", "np", "nr",
    "nra", "nrw", "ntt", "nu", "nyc", "nz",
    "obi", "observer", "office", "okinawa", "olayan", "olayangroup",
    "ollo", "om", "omega", "one", "ong", "onl",
    "online", "ooo", "open", "oracle", "orange", "org",
    "organic", "origins", "osaka", "otsuka", "ott", "ovh",
    "pa", "page", "panasonic", "paris", "pars", "partners",
    "parts", "party", "pay", "pccw", "pe", "pet",
    "pf", "pfizer", "pg", "ph", "pharmacy", "phd",
    "philips", "phone", "photo", "photography", "photos", "physio",
    "pics", "pictet", "pictures", "pid", "pin", "ping",
    "pink", "pioneer", "pizza", "pk", "pl", "place",
    "play", "playstation", "plumbing", "plus", "pm", "pn",
    "pnc", "pohl", "poker", "politie", "porn", "post",
    "pr", "pramerica", "praxi", "press", "prime", "pro",
    "prod", "productions", "prof", "progressive", "promo", "properties",
    "property", "protection", "pru", "prudential", "ps", "pt",
    "pub", "pw", "pwc", "py", "qa", "qpon",
    "quebec", "quest", "racing", "radio", "re", "read",
    "realestate", "realtor", "realty", "recipes", "red", "redstone",
    "redumbrella", "rehab", "reise", "reisen", "reit", "reliance",
    "ren", "rent", "rentals", "repair", "report", "republican",
    "rest", "restaurant", "review", "reviews", "rexroth", "rich",
    "richardli", "ricoh", "ril", "rio", "rip", "ro",
    "rocks", "rodeo", "rogers", "room", "rs", "rsvp",
    "ru", "rugby", "ruhr", "run", "rw", "rwe",
    "ryukyu", "sa", "saarland", "safe", "safety", "sakura",
    "sale", "salon", "samsclub", "samsung", "sandvik", "sandvikcoromant",
    "sanofi", "sap", "sarl", "sas", "save", "saxo",
    "sb", "sbi", "sbs", "sc", "scb", "schaeffler",
    "schmidt", "scholarships", "school", "schule", "schwarz", "science",
    "scot", "sd", "se", "search", "seat", "secure",
    "security", "seek", "select", "sener", "services", "seven",
    "sew", "sex", "sexy", "sfr", "sg", "sh",
    "shangrila", "sharp", "shaw", "shell", "shia", "shiksha",
    "shoes", "shop", "shopping", "shouji", "show", "si",
    "silk", "sina", "singles", "site", "sj", "sk",
    "ski", "skin", "sky", "skype", "sl", "sling",
    "sm", "smart", "smile", "sn", "sncf", "so",
    "soccer", "social", "softbank", "software", "sohu", "solar",
    "solutions", "song", "sony", "soy", "spa", "space",
    "sport", "spot", "sr", "srl", "ss", "st",
    "stada", "staples", "star", "statebank", "statefarm", "stc",
    "stcgroup", "stockholm", "storage", "store", "stream", "studio",
    "study", "style", "su", "sucks", "supplies", "supply",
    "support", "surf", "surgery", "suzuki", "sv", "swatch",
    "swiss", "sx", "sy", "sydney", "systems", "sz",
    "tab", "taipei", "talk", "taobao", "target", "tatamotors",
    "tatar", "tattoo", "tax", "taxi", "tc", "tci",
    "td", "tdk", "team", "tech", "technology", "tel",
    "temasek", "tennis", "teva", "tf", "tg", "th",
    "thd", "theater", "theatre", "tiaa", "tickets", "tienda",
    "tips", "tires", "tirol", "tj", "tjmaxx", "tjx",
    "tk", "tkmaxx", "tl", "tm", "tmall", "tn",
    "to", "today", "tokyo", "tools", "top", "toray",
    "toshiba", "total", "tours", "town", "toyota", "toys",
    "tr", "trade", "trading", "training", "travel", "travelers",
    "travelersinsurance", "trust", "trv", "tt", "tube", "tui",
    "tunes", "tushu", "tv", "tvs", "tw", "tz",
    "ua", "ubank", "ubs", "ug", "uk", "unicom",
    "university", "uno", "uol", "ups", "us", "uy",
    "uz", "va", "vacations", "vana", "vanguard", "vc",
    "ve", "vegas", "ventures", "verisign", "versicherung", "vet",
    "vg", "vi", "viajes", "video", "vig", "viking",
    "villas", "vin", "vip", "virgin", "visa", "vision",
    "viva", "vivo", "vlaanderen", "vn", "vodka", "volvo",
    "vote", "voting", "voto", "voyage", "vu", "wales",
    "walmart", "walter", "wang", "wanggou", "watch", "watches",
    "weather", "weatherchannel", "webcam", "weber", "website", "wed",
    "wedding", "weibo", "weir", "wf", "whoswho", "wien",
    "wiki", "williamhill", "win", "windows", "wine", "winners",
    "wme", "wolterskluwer", "woodside", "work", "works", "world",
    "wow", "ws", "wtc", "wtf", "xbox", "xerox",
    "xihuan", "xin", "xnaac", "xnadxhks", "xnae", "xnais",
    "xnaoa", "xnaqecdra", "xnasehdb", "xnaswg", "xnbcd", "xnbckbadrec",
    "xnbftacc", "xnbrcyl", "xnbrjc", "xnbstm", "xnbwferd", "xncavg",
    "xncbrg", "xncckbb", "xncckwcxetd", "xncda", "xncgbki", "xnckeb",
    "xnclchceabgagcd", "xnczrb", "xnczrst", "xnczrud", "xndacjb", "xndalf",
    "xndbqa", "xndbrkce", "xndsg", "xneac", "xnebe", "xneckvdtcd",
    "xnefvyh", "xnetu", "xnfctk", "xnfhbei", "xnfiqb", "xnfiqchs",
    "xnfiqss", "xnfiqzs", "xnfjqa", "xnflwe", "xnfpcrjcd", "xnfrzg",
    "xnfzccec", "xnfzysduvgm", "xngbrim", "xngckrff", "xngecrjc", "xngkate",
    "xngxxc", "xnhbregeve", "xnhbrjc", "xnhbrjcc", "xnhcrjc", "xnhxte",
    "xnibbaae", "xnimrn", "xnioai", "xnjaef", "xnjamh", "xnjlqnrg",
    "xnjvrm", "xnjwg", "xnkcrxdxa", "xnkprwd", "xnkpryd", "xnkputi",
    "xnkrta", "xnlacc", "xnlgbbatadj", "xnmgbaaejt", "xnmgbaafa", "xnmgbaamah",
    "xnmgbabbd", "xnmgbacbbna", "xnmgbahahjkrd", "xnmgbaiazgqpj", "xnmgbawbf", "xnmgbayhgpa",
    "xnmgbbha", "xnmgbbhae", "xnmgbcaazcg", "xnmgbcadzdo", "xnmgbcpqgpaa", "xnmgberpadar",
    "xnmgbgua", "xnmgbiecexp", "xnmgbplfh", "xnmgbtdhd", "xnmgbtxb", "xnmgbxcdab",
    "xnmixf", "xnmkbuc", "xnmxtqm", "xnngbcazd", "xnngbeea", "xnngbrx",
    "xnnode", "xnnqvf", "xnnqvfsema", "xnnyqya", "xnocwh", "xnogbpffl",
    "xnotud", "xnpacf", "xnpai", "xnpgbsdh", "xnpssyu", "xnpxuk",
    "xnqc", "xnqcea", "xnqckapmc", "xnqjybc", "xnqqbxl", "xnqqwa",
    "xnqwg", "xnqxaa", "xnqxam", "xnqxd", "xnrhqvg", "xnrovub",
    "xnrry", "xnrvceame", "xnsbrjc", "xnscrjc", "xnsesg", "xnsujbgsg",
    "xntba", "xntckwe", "xntiqxqyj", "xntzmg", "xnunupy", "xnvermgensberaterctb",
    "xnvermgensberatungpwb", "xnvhquv", "xnvuqb", "xnwgbhc", "xnwgbla", "xnwrelfhudnra",
    "xnwrsl", "xnxhqb", "xnxkcalhyea", "xnxkcdlaeeh", "xnyaa", "xnyaaq",
    "xnyfroio", "xnygbiammx", "xnzfrb", "xxx", "xyz", "yachts",
    "yahoo", "yamaxun", "yandex", "ye", "yodobashi", "yoga",
    "yokohama", "you", "youtube", "yt", "yun", "za",
    "zappos", "zara", "zero", "zip", "zm", "zone",
    "zuerich", "zw",
};


bool is_tld(std::string_view label) {
    if (label.empty() || label.size() > 24) {
        return false;
    }
    char lowered[24];
    for (size_t i = 0; i < label.size(); ++i) {
        const char c = label[i];
        lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    const std::string_view key(lowered, label.size());
    const auto *begin = std::begin(kTopLevelDomains);
    const auto *end = std::end(kTopLevelDomains);
    const auto *found = std::lower_bound(begin, end, key);
    return found != end && *found == key;
}

// ---------------------------------------------------------------------------
// Character classes (RFC 3986 / 5322, all ASCII)
// ---------------------------------------------------------------------------

bool is_ascii_alpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
bool is_ascii_alnum(char c) noexcept { return is_ascii_alpha(c) || is_ascii_digit(c); }
bool is_ascii_hex(char c) noexcept {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_unreserved(char c) noexcept {
    return is_ascii_alnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

bool is_percent_encoded(std::string_view text, size_t pos) noexcept {
    return pos + 2 < text.size() && text[pos] == '%' && is_ascii_hex(text[pos + 1]) &&
           is_ascii_hex(text[pos + 2]);
}

/// Consumes `(unreserved | pct-encoded | sub-delims | extra)*`.
size_t consume_component(std::string_view text, size_t pos, std::string_view extra) {
    while (pos < text.size()) {
        const char c = text[pos];
        if (is_unreserved(c) || extra.find(c) != std::string_view::npos) {
            ++pos;
        } else if (is_percent_encoded(text, pos)) {
            pos += 3;
        } else {
            break;
        }
    }
    return pos;
}

/// `URIquery` / `URIfragment` body: `(unreserved | pct-encoded | sub-delims | [:@/?])*`,
/// i.e. a component that additionally accepts the `URIsubDelims` set.
size_t consume_query_body(std::string_view text, size_t pos) {
    // `extra` is consulted by `consume_component`, so the sub-delimiters are simply
    // added to it: `! $ & ' ( ) * + , ; =` plus `: @ / ?`.
    static constexpr std::string_view kQueryExtra = "!$&'()*+,;=:@/?";
    return consume_component(text, pos, kQueryExtra);
}

bool match_ci(std::string_view text, size_t pos, std::string_view lower) noexcept {
    if (pos + lower.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i < lower.size(); ++i) {
        char c = text[pos + i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        if (c != lower[i]) {
            return false;
        }
    }
    return true;
}

/// `URIlogin` = `segment (":" segment)? "@"`; returns the position after `@`, or 0.
size_t consume_login(std::string_view text, size_t pos) {
    const size_t first = consume_component(text, pos, ":@&=;");
    if (first < text.size() && text[first] == ':') {
        const size_t second = consume_component(text, first + 1, ":@&=;");
        if (second < text.size() && text[second] == '@') {
            return second + 1;
        }
    }
    if (first < text.size() && text[first] == '@') {
        return first + 1;
    }
    return 0;
}

/// `IPv4Address` = octet "." octet "." octet "." octet; returns the end or 0.
size_t consume_ipv4(std::string_view text, size_t pos) {
    size_t cursor = pos;
    for (int octet = 0; octet < 4; ++octet) {
        const size_t start = cursor;
        int value = 0;
        while (cursor < text.size() && is_ascii_digit(text[cursor]) && cursor - start < 3) {
            value = value * 10 + (text[cursor] - '0');
            ++cursor;
        }
        if (cursor == start || value > 255) {
            return 0;
        }
        if (octet < 3) {
            if (cursor >= text.size() || text[cursor] != '.') {
                return 0;
            }
            ++cursor;
        }
    }
    return cursor;
}

/// `"[" IPv6Address "]"`; returns the end or 0.
///
/// **Deviation:** the address is validated structurally (1-8 hex groups with at
/// most one `::`) instead of with the reference implementation's fully expanded
/// DFA; the embedded-IPv4 tail form is not accepted inside brackets.
size_t consume_ipv6_bracketed(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '[') {
        return 0;
    }
    size_t cursor = pos + 1;
    int groups = 0;
    bool compressed = false;
    while (cursor < text.size()) {
        const char c = text[cursor];
        if (c == ']') {
            if (groups == 0 && !compressed) {
                return 0;
            }
            if (!compressed && groups != 8) {
                return 0;
            }
            return cursor + 1;
        }
        if (c == ':') {
            if (cursor + 1 < text.size() && text[cursor + 1] == ':') {
                if (compressed) {
                    return 0;
                }
                compressed = true;
                cursor += 2;
                continue;
            }
            ++cursor;
            continue;
        }
        if (!is_ascii_hex(c)) {
            return 0;
        }
        size_t length = 0;
        while (cursor < text.size() && is_ascii_hex(text[cursor]) && length < 4) {
            ++cursor;
            ++length;
        }
        ++groups;
        if (groups > 8) {
            return 0;
        }
    }
    return 0;
}

/// One `DomainLabel`; returns its end, or `pos` when it is empty.
size_t consume_domain_label(std::string_view text, size_t pos) {
    if (pos >= text.size() || !is_ascii_alnum(text[pos])) {
        return pos;
    }
    size_t cursor = pos + 1;
    size_t last_alnum = cursor;
    while (cursor < text.size() && (is_ascii_alnum(text[cursor]) || text[cursor] == '-')) {
        ++cursor;
        if (is_ascii_alnum(text[cursor - 1])) {
            last_alnum = cursor;
        }
    }
    return last_alnum;
}

/// `DomainLabel ("." DomainLabel)*`; returns the end (>= `pos`), the last label and
/// how many labels the sequence has.
size_t consume_domain_labels(std::string_view text, size_t pos, std::string_view &last_label, size_t &label_count) {
    size_t cursor = consume_domain_label(text, pos);
    label_count = 0;
    if (cursor == pos) {
        return pos;
    }
    label_count = 1;
    size_t label_start = pos;
    last_label = std::string_view(text.data() + label_start, cursor - label_start);
    while (cursor + 1 < text.size() && text[cursor] == '.') {
        const size_t next = consume_domain_label(text, cursor + 1);
        if (next == cursor + 1) {
            break;
        }
        label_start = cursor + 1;
        last_label = std::string_view(text.data() + label_start, next - label_start);
        cursor = next;
        ++label_count;
    }
    return cursor;
}

/// `URIhostLoose`; returns the end or 0.
size_t consume_host_loose(std::string_view text, size_t pos) {
    if (const size_t ipv6 = consume_ipv6_bracketed(text, pos); ipv6 != 0) {
        return ipv6;
    }
    if (const size_t ipv4 = consume_ipv4(text, pos); ipv4 != 0) {
        return ipv4;
    }
    std::string_view ignored;
    size_t labels = 0;
    const size_t end = consume_domain_labels(text, pos, ignored, labels);
    return end == pos ? 0 : end;
}

/// `URIhostStrict`; returns the end or 0.  The last label has to be a real TLD.
size_t consume_host_strict(std::string_view text, size_t pos) {
    if (const size_t ipv6 = consume_ipv6_bracketed(text, pos); ipv6 != 0) {
        return ipv6;
    }
    if (const size_t ipv4 = consume_ipv4(text, pos); ipv4 != 0) {
        return ipv4;
    }
    std::string_view last_label;
    size_t labels = 0;
    const size_t end = consume_domain_labels(text, pos, last_label, labels);
    // `DomainNameStrict` is `DomainLabelSequence "." ASCIITLD`, so a single label is
    // never a domain: that keeps two letter words that happen to be country TLDs
    // (`to`, `ee`, `me`, ...) out of the URL token.
    if (end == pos || labels < 2 || !is_tld(last_label)) {
        return 0;
    }
    // The grammar accepts a trailing root (empty) domain.
    return end < text.size() && text[end] == '.' ? end + 1 : end;
}

/// `URIport` = ":" 1*5DIGIT; returns the end or 0.
size_t consume_port(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != ':') {
        return 0;
    }
    size_t cursor = pos + 1;
    size_t digits = 0;
    while (cursor < text.size() && is_ascii_digit(text[cursor]) && digits < 5) {
        ++cursor;
        ++digits;
    }
    return digits == 0 ? 0 : cursor;
}

/// `HTTPpath` = 1*("/" segment); returns the end, or `pos` when there is none.
size_t consume_http_path(std::string_view text, size_t pos) {
    size_t cursor = pos;
    size_t segments = 0;
    while (cursor < text.size() && text[cursor] == '/') {
        cursor = consume_component(text, cursor + 1, ";:@&=");
        ++segments;
    }
    return segments == 0 ? pos : cursor;
}

/// `FTPorFILEpath` = "/" segment *("/" segment); returns the end or `pos`.
size_t consume_ftp_path(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '/') {
        return pos;
    }
    size_t cursor = pos;
    while (cursor < text.size() && text[cursor] == '/') {
        cursor = consume_component(text, cursor + 1, "?:@&=");
    }
    return cursor;
}

/// `URIquery`; returns the end or `pos`.
size_t consume_query(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '?') {
        return pos;
    }
    return consume_query_body(text, pos + 1);
}

/// `URIfragment`; returns the end or `pos`.
size_t consume_fragment(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '#') {
        return pos;
    }
    return consume_query_body(text, pos + 1);
}

/// `[-\w]`: the character class the "this is not a URL after all" rules look at.
bool is_url_word_char(char c) noexcept { return is_ascii_alnum(c) || c == '_' || c == '-'; }

/// The end of the URL starting at `pos`, or 0.
size_t match_url(std::string_view text, size_t pos) {
    const size_t size = text.size();

    if (match_ci(text, pos, "http") && pos + 4 <= size) {
        size_t cursor = pos + 4;
        if (cursor < size && (text[cursor] == 's' || text[cursor] == 'S')) {
            ++cursor;
        }
        if (cursor + 3 <= size && text.compare(cursor, 3, "://") == 0) {
            cursor += 3;
            const size_t login = consume_login(text, cursor);
            if (login != 0) {
                cursor = login;
            }
            const size_t host = consume_host_loose(text, cursor);
            if (host == 0) {
                return 0;
            }
            cursor = host;
            if (const size_t port = consume_port(text, cursor); port != 0) {
                cursor = port;
            }
            cursor = consume_http_path(text, cursor);
            cursor = consume_query(text, cursor);
            cursor = consume_fragment(text, cursor);
            return cursor;
        }
    }

    if (match_ci(text, pos, "ftp") && pos + 6 <= size && text.compare(pos + 3, 3, "://") == 0) {
        size_t cursor = pos + 6;
        const size_t login = consume_login(text, cursor);
        if (login != 0) {
            cursor = login;
        }
        const size_t host = consume_host_loose(text, cursor);
        if (host == 0) {
            return 0;
        }
        cursor = host;
        if (const size_t port = consume_port(text, cursor); port != 0) {
            cursor = port;
        }
        const size_t path = consume_ftp_path(text, cursor);
        if (path == cursor) {
            return 0;
        }
        cursor = path;
        // `FTPtype` = ";" "type=" [aAiIdD]
        if (cursor + 7 <= size && text[cursor] == ';' && match_ci(text, cursor + 1, "type=")) {
            const char kind = text[cursor + 6];
            if (kind == 'a' || kind == 'A' || kind == 'i' || kind == 'I' || kind == 'd' || kind == 'D') {
                cursor += 7;
            }
        }
        cursor = consume_fragment(text, cursor);
        return cursor;
    }

    if (match_ci(text, pos, "file") && pos + 7 <= size && text.compare(pos + 4, 3, "://") == 0) {
        size_t cursor = pos + 7;
        if (const size_t host = consume_host_loose(text, cursor); host != 0) {
            cursor = host;
        }
        const size_t path = consume_ftp_path(text, cursor);
        if (path == cursor) {
            return 0;
        }
        cursor = path;
        cursor = consume_fragment(text, cursor);
        return cursor;
    }

    // `HTTPurlNoScheme`: a strict host followed by a port, a path, a query or a
    // fragment.  A bare host is accepted too, unless the next character would make
    // it part of a longer word (LUCENE-5391).
    const size_t host = consume_host_strict(text, pos);
    if (host == 0) {
        return 0;
    }
    size_t cursor = host;
    bool extended = false;
    if (const size_t port = consume_port(text, cursor); port != 0) {
        cursor = port;
        extended = true;
    }
    const size_t path = consume_http_path(text, cursor);
    if (path != cursor) {
        cursor = path;
        extended = true;
    }
    const size_t query = consume_query(text, cursor);
    if (query != cursor) {
        cursor = query;
        extended = true;
    }
    const size_t fragment = consume_fragment(text, cursor);
    if (fragment != cursor) {
        cursor = fragment;
        extended = true;
    }
    if (!extended && cursor < size && is_url_word_char(text[cursor])) {
        return 0;
    }
    return cursor;
}

bool is_email_atom(char c) noexcept {
    if (is_ascii_alnum(c)) {
        return true;
    }
    switch (c) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '/':
        case '=':
        case '?':
        case '^':
        case '_':
        case '`':
        case '{':
        case '|':
        case '}':
        case '~':
            return true;
        default:
            return false;
    }
}

/// `EMAILlabel` = atom+ | quoted-string.
size_t consume_email_label(std::string_view text, size_t pos) {
    if (pos < text.size() && text[pos] == '"') {
        size_t cursor = pos + 1;
        while (cursor < text.size()) {
            if (text[cursor] == '\\') {
                cursor += 2;
                continue;
            }
            if (text[cursor] == '"') {
                return cursor + 1;
            }
            if (static_cast<unsigned char>(text[cursor]) < 0x20) {
                return pos;
            }
            ++cursor;
        }
        return pos;
    }
    size_t cursor = pos;
    while (cursor < text.size() && is_email_atom(text[cursor])) {
        ++cursor;
    }
    return cursor;
}

/// `EMAILlocalPart` = label ("." label)*.
size_t consume_email_local_part(std::string_view text, size_t pos) {
    size_t cursor = consume_email_label(text, pos);
    if (cursor == pos) {
        return pos;
    }
    while (cursor + 1 < text.size() && text[cursor] == '.') {
        const size_t next = consume_email_label(text, cursor + 1);
        if (next == cursor + 1) {
            break;
        }
        cursor = next;
    }
    return cursor;
}

/// `EMAILbracketedHost` = "[" ... "]"; returns the end or 0.
size_t consume_email_bracketed_host(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '[') {
        return 0;
    }
    size_t cursor = pos + 1;
    while (cursor < text.size()) {
        const char c = text[cursor];
        if (c == '\\') {
            cursor += 2;
            continue;
        }
        if (c == ']') {
            return cursor + 1;
        }
        if (c == '[') {
            return 0;
        }
        ++cursor;
    }
    return 0;
}

/// The end of the email address starting at `pos`, or 0.
size_t match_email(std::string_view text, size_t pos) {
    const size_t local_end = consume_email_local_part(text, pos);
    if (local_end == pos || local_end >= text.size() || text[local_end] != '@') {
        return 0;
    }
    const size_t domain_start = local_end + 1;
    if (const size_t bracketed = consume_email_bracketed_host(text, domain_start); bracketed != 0) {
        return bracketed;
    }
    const size_t domain = consume_host_strict(text, domain_start);
    if (domain == 0) {
        return 0;
    }
    return domain;
}

} // namespace

UrlEmailTokenizer::UrlEmailTokenizer() {
    attributes_.add_attribute<CharTermAttribute>();
    attributes_.add_attribute<OffsetAttribute>();
    attributes_.add_attribute<PositionIncrementAttribute>();
    attributes_.add_attribute<PositionLengthAttribute>();
    type_ = attributes_.add_attribute<TypeAttribute>();
}

std::string_view UrlEmailTokenizer::type_name(UrlEmailTokenType type) noexcept {
    switch (type) {
        case UrlEmailTokenType::kNum:
            return StandardTokenizerTypes::NUM;
        case UrlEmailTokenType::kUrl:
            return "<URL>";
        case UrlEmailTokenType::kEmail:
            return "<EMAIL>";
        case UrlEmailTokenType::kAlphanum:
        default:
            return StandardTokenizerTypes::ALPHANUM;
    }
}

void UrlEmailTokenizer::append_word_pieces(std::string_view text, size_t start, size_t end) {
    if (start >= end) {
        return;
    }
    StandardTokenizer words;
    if (!words.set_reader(text.substr(start, end - start)).has_value()) {
        return;
    }
    if (!words.reset().has_value()) {
        return;
    }
    while (true) {
        auto more = words.increment_token();
        if (!more.has_value() || !more.value()) {
            break;
        }
        const auto *term = words.attributes().get_attribute<CharTermAttribute>();
        const auto *offset = words.attributes().get_attribute<OffsetAttribute>();
        if (term == nullptr || offset == nullptr) {
            break;
        }
        // The word rules already classify `<NUM>` (which also covers `4.5`).
        const auto *type = words.attributes().get_attribute<TypeAttribute>();
        const bool numeric = type != nullptr && type->type() == StandardTokenizerTypes::NUM;
        pieces_.push_back(Piece{start + offset->start_offset(), start + offset->end_offset(),
                                numeric ? UrlEmailTokenType::kNum : UrlEmailTokenType::kAlphanum});
    }
}

VoidResult UrlEmailTokenizer::reset() {
    auto result = Tokenizer::reset();
    if (!result.has_value()) {
        return result;
    }
    pieces_.clear();
    next_ = 0;
    current_type_ = UrlEmailTokenType::kAlphanum;

    const std::string_view text(input_);
    size_t piece_start = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t email = match_email(text, pos);
        const size_t url = match_url(text, pos);
        const size_t end = std::max(email, url);
        if (end == 0) {
            ++pos;
            continue;
        }
        append_word_pieces(text, piece_start, pos);
        pieces_.push_back(Piece{pos, end, url >= email ? UrlEmailTokenType::kUrl : UrlEmailTokenType::kEmail});
        pos = end;
        piece_start = end;
    }
    append_word_pieces(text, piece_start, text.size());
    return {};
}

Result<bool> UrlEmailTokenizer::increment_token() {
    if (next_ >= pieces_.size()) {
        return false;
    }
    const Piece &piece = pieces_[next_];
    ++next_;

    attributes_.clear_attributes();
    auto *term = attributes_.add_attribute<CharTermAttribute>();
    auto *offset = attributes_.add_attribute<OffsetAttribute>();
    auto *pos_inc = attributes_.add_attribute<PositionIncrementAttribute>();
    auto *pos_len = attributes_.add_attribute<PositionLengthAttribute>();
    term->set_text(std::string_view(input_).substr(piece.start, piece.end - piece.start));
    offset->set_offset(static_cast<uint32_t>(piece.start), static_cast<uint32_t>(piece.end));
    pos_inc->set_position_increment(1);
    pos_len->set_position_length(1);
    type_->set_type(type_name(piece.type));
    current_type_ = piece.type;
    return true;
}

} // namespace lucene
