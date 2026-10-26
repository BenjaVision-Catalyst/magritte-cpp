#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool b) {
    ++checks;
    if (!b)
        throw Error("Container protocol mismatch");
}
template <class F> bool fails(F f) {
    try {
        f();
    } catch (const Error &) {
        return true;
    }
    return false;
}
std::shared_ptr<Description> field(const std::string &key, int priority = 0,
                                   const std::string &cls = "MAStringDescription") {
    auto d = std::make_shared<Description>();
    d->id = key;
    d->key = key;
    d->className = cls;
    d->properties["priority"] = Value(std::int64_t(priority));
    return d;
}
} // namespace
int main() {
    try {
        std::ifstream input(std::string(FIXTURES) + "/container-protocol-reference.json");
        J fixture;
        input >> fixture;
        for (const auto &expected : fixture.at("cases")) {
            Description d;
            d.id = "root";
            d.className = expected.at("class");
            if (!expected.at("empty").get<bool>())
                d.children = {field("a", 30), field("b", 10), field("c", 20)};
            auto fallback = field("fallback");
            J row = {{"class", d.className}, {"empty", d.children.empty()}};
            J trace = J::array();
            forEachDescription(d, [&](const auto &item) { trace.push_back(item->key); });
            row["each"] = trace;
            trace = J::array();
            forEachDescriptionSeparated(
                d, [&](const auto &item) { trace.push_back(item->key); },
                [&] { trace.push_back("separator"); });
            row["separated"] = trace;
            trace = J::array();
            forEachDescriptionIndexed(
                d, [&](auto i, const auto &item) { trace.push_back(J::array({i, item->key})); });
            row["indexed"] = trace;
            row["fold"] =
                reduceDescriptions(d, std::string("start"), [](auto acc, const auto &item) {
                    return acc + ":" + item->key;
                });
            std::vector<int> other;
            for (std::size_t i = 0; i < d.children.size(); ++i)
                other.push_back(int(i + 1));
            trace = J::array();
            forEachDescriptionPaired(d, other, [&](const auto &item, int n) {
                trace.push_back(J::array({item->key, n}));
            });
            row["paired"] = trace;
            int missing = 0;
            auto first = childAt(d, 0, [&] {
                ++missing;
                return fallback;
            });
            row["first"] = first->key;
            row["firstFallbackCalls"] = missing;
            missing = 0;
            first = childAt(d, d.children.size(), [&] {
                ++missing;
                return fallback;
            });
            row["absent"] = first->key;
            row["absentFallbackCalls"] = missing;
            auto equal = field("a");
            equal->properties["label"] = Value("Different label");
            row["includesEqual"] = includesDescription(d, *equal);
            row["includesOther"] = includesDescription(d, *field("a", 0, "MANumberDescription"));
            for (const std::string op : {"all", "any", "none", "detect", "missing"}) {
                trace = J::array();
                missing = 0;
                auto predicate = [&](const auto &item) {
                    trace.push_back(item.key);
                    return op != "missing" && item.priority() >= 20;
                };
                J answer;
                if (op == "all")
                    answer = allDescriptionsSatisfy(d, predicate);
                else if (op == "any")
                    answer = anyDescriptionSatisfies(d, predicate);
                else if (op == "none")
                    answer = noDescriptionSatisfies(d, predicate);
                else
                    answer = detectDescription(d, predicate, [&] {
                                 ++missing;
                                 return fallback;
                             })->key;
                row[op] = {{"result", answer}, {"trace", trace}, {"fallbackCalls", missing}};
            }
            trace = J::array();
            row["callbackError"] = fails([&] {
                forEachDescription(d, [&](const auto &item) {
                    trace.push_back(item->key);
                    if (item->key == "b")
                        throw Error("callback");
                });
            });
            row["callbackTrace"] = trace;
            trace = J::array();
            other.push_back(42);
            row["pairSizeError"] = fails([&] {
                forEachDescriptionPaired(d, other,
                                         [&](const auto &, int n) { trace.push_back(n); });
            });
            row["pairSizeTrace"] = trace;
            row["atError"] = fails([&] { childAt(d, d.children.size()); });
            row["detectError"] =
                fails([&] { detectDescription(d, [](const auto &) { return false; }); });
            if (row != expected) {
                std::cerr << "Expected " << expected << "\nActual " << row << '\n';
                throw Error("GT reference mismatch");
            }
            ++checks;
        }
        Description d;
        d.id = "root";
        d.className = "MAContainer";
        auto a = field("a"), b = field("b");
        d.children = {a, b, a};
        check(childAt(d, 0) == a && childAt(d, 2) == a);
        check(detectDescription(d, [](const auto &x) { return x.key == "a"; }) == a);
        check(childAt(d, 99, [] { return nullptr; }) == nullptr);
        check(detectDescription(
                  d, [](const auto &) { return false; }, [] { return nullptr; }) == nullptr);
        int visits = 0;
        forEachDescription(d, [&](const auto &) { ++visits; });
        check(visits == 3);
        check(reduceDescriptions(d, 0, [](int n, const auto &) { return n + 1; }) == 3);
        Limits small;
        small.items = 2;
        check(fails([&] { forEachDescription(d, [&](const auto &) { ++visits; }, small); }));
        check(visits == 3);
        small = Limits{};
        small.visits = 2;
        check(fails([&] { anyDescriptionSatisfies(d, [](const auto &) { return true; }, small); }));
        small = Limits{};
        small.visits = 3;
        check(fails([&] { includesDescription(d, *field("absent"), small); }));
        Description scalar;
        check(fails([&] { enumerationChildren(scalar); }));
        check(fails([&] {
            childAt(d, 99, []() -> std::shared_ptr<Description> { throw Error("fallback"); });
        }));
        check(fails(
            [&] { detectDescription(d, [](const auto &) -> bool { throw Error("predicate"); }); }));
        check(fails([&] {
            forEachDescriptionSeparated(d, [](const auto &) {}, [] { throw Error("separator"); });
        }));
        check(fails([&] {
            reduceDescriptions(d, 0, [](int, const auto &) -> int { throw Error("fold"); });
        }));
        check(fails([&] {
            forEachDescriptionPaired(d, std::vector<int>{1, 2, 3},
                                     [](const auto &, int) { throw Error("pair"); });
        }));
        auto nested = std::make_shared<Description>();
        nested->className = "MAContainer";
        nested->id = "nested";
        nested->children = {a};
        d.children = {nested};
        visits = 0;
        forEachDescription(d, [&](const auto &item) {
            check(item == nested);
            ++visits;
        });
        check(visits == 1);
        d.children = {nullptr};
        check(fails([&] { enumerationChildren(d); }));
        std::cout << checks << " container protocol checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
