#include <magritte/magritte.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
using namespace magritte;
using J = nlohmann::json;
namespace {
int checks = 0;
void check(bool ok, const std::string &message = "Pragma mismatch") {
    ++checks;
    if (!ok)
        throw Error(message);
}
template <class F> void fails(F f) {
    bool failed = false;
    try {
        f();
    } catch (const Error &) {
        failed = true;
    }
    check(failed, "Expected bounded failure");
}
std::string text(const Description &d) { return std::get<std::string>(d.property("label").data); }
DescriptionCopy own(std::shared_ptr<Description> d) {
    return {d,
            d ? std::vector<std::shared_ptr<Description>>{d}
              : std::vector<std::shared_ptr<Description>>{},
            {}};
}
struct Probe : DictionaryModel {
    std::string mode;
    int containers = 0;
    Probe *peer = nullptr;
    std::vector<std::string> events;
    explicit Probe(std::string m = "simple") : mode(std::move(m)) {
        values = {{"name", Value("Ada")}, {"score", Value(std::int64_t(7))}};
    }
};
std::shared_ptr<Description> field(DescriptionRequest &r, std::string key, std::string label,
                                   std::int64_t priority) {
    auto d = std::make_shared<Description>();
    d->id = r.builder.freshId();
    d->key = std::move(key);
    d->properties = {{"label", Value(std::move(label))}, {"priority", Value(priority)}};
    return d;
}
void setup() {
    ClassRegistry::global().registerClasses(
        {{"MAPragmaProbeBase", "Object", false, "Probe Base"},
         {"MAPragmaProbeTagged", "MAPragmaProbeBase", false, "Probe Tagged"},
         {"MAPragmaProbePlain", "MAPragmaProbeBase", false, "Probe Plain"},
         {"MAPragmaProbeDuplicate", "MAPragmaProbeBase", false, "Probe Duplicate"},
         {"MAPragmaProbeContainer", "MAPragmaProbeBase", false, "Probe Container"},
         {"MAPragmaProbeEmpty", "Object", false, "Empty"}});
    auto &registry = DescriptionMethodRegistry::global();
    auto install = [&](std::string cls, std::string sel, std::size_t arity,
                       std::vector<DescriptionPragma> tags,
                       auto op) { registry.install({cls, sel, arity, std::move(tags), op}); };
    install("MAPragmaProbeBase", "descriptionContainer", 0, {}, [](DescriptionRequest &r, auto) {
        auto &p = dynamic_cast<Probe &>(*r.target.model);
        ++p.containers;
        p.events.push_back("container");
        check(DescribedObjectScope::current()->identity == r.target.identity,
              "Container dynamic object");
        if (p.mode == "container-error")
            throw Error("provider");
        if (p.mode == "container-recursive")
            return r.builder.forTarget(r.target, "ignored");
        if (p.mode == "nil-container")
            return DescriptionCopy{};
        auto d = field(r, "", ClassRegistry::global().find(r.target.className)->label, 0);
        d->className = p.mode == "scalar-container" ? "MAStringDescription" : "MAPriorityContainer";
        return own(d);
    });
    install("MAPragmaProbeBase", "decorateContainer:", 1, {{"magritteContainer", {}}},
            [](DescriptionRequest &r, auto d) {
                auto &p = dynamic_cast<Probe &>(*r.target.model);
                check(DescribedObjectScope::current()->identity == r.target.identity);
                p.events.push_back("decorate");
                if (d) {
                    d->properties["label"] = Value(text(*d) + "C");
                    if (p.mode == "seed")
                        d->children.push_back(field(r, "seed", "Seed", 5));
                }
                return DescriptionCopy{};
            });
    auto name = [](std::string label) {
        return [label](DescriptionRequest &r, auto) {
            auto &p = dynamic_cast<Probe &>(*r.target.model);
            p.events.push_back("name");
            if (p.mode == "nil-name")
                return DescriptionCopy{};
            return own(field(r, "name", label, 20));
        };
    };
    install("MAPragmaProbeBase", "descriptionName", 0, {{"magritteDescription", {}}}, name("Name"));
    install("MAPragmaProbeBase", "descriptionNumber", 0, {{"magritteDescription", {}}},
            [](DescriptionRequest &r, auto) {
                auto &p = dynamic_cast<Probe &>(*r.target.model);
                p.events.push_back("number");
                if (p.mode == "getter-error")
                    throw Error("getter");
                auto d = field(r, "score", "Score", 10);
                d->className = "MANumberDescription";
                return own(d);
            });
    install("MAPragmaProbeBase", "descriptionSelf", 0, {{"magritteDescription", {}}},
            [](DescriptionRequest &r, auto) {
                auto &p = dynamic_cast<Probe &>(*r.target.model);
                if (p.mode != "recursive")
                    return DescriptionCopy{};
                auto cached = r.builder.forTarget(r.target, "ignored");
                check(cached.root->children.empty(), "Recursive cache inserted before fields");
                auto d = field(r, "self", "Self", 30);
                d->className = "MAToOneRelationDescription";
                d->reference = cached.root;
                d->classes = {r.target.className};
                return DescriptionCopy{d, {cached.root}, cached.lifetime};
            });
    auto tag = std::vector<DescriptionPragma>{
        {"magritteDescription:", {Value(Value::Symbol{"descriptionName"})}}};
    install("MAPragmaProbeBase", "aName:", 1, tag, [](DescriptionRequest &r, auto d) {
        auto &p = dynamic_cast<Probe &>(*r.target.model);
        check(d->definingContext != nullptr, "Context precedes extensions");
        p.events.push_back("a:" + text(*d));
        if (p.mode == "replacement")
            return own(field(r, "name", "Replacement", 20));
        if (p.mode == "nil-first")
            return DescriptionCopy{};
        d->properties["label"] = Value(text(*d) + "A");
        return own(d);
    });
    install("MAPragmaProbeBase", "zName:", 1, tag, [](DescriptionRequest &r, auto d) {
        auto &p = dynamic_cast<Probe &>(*r.target.model);
        p.events.push_back("z:" + text(*d));
        if (p.mode == "extension-error")
            throw Error("extension");
        if (p.mode == "nil-last")
            return DescriptionCopy{};
        d->properties["label"] = Value(text(*d) + "Z");
        return own(d);
    });
    install("MAPragmaProbeTagged", "descriptionName", 0, {{"magritteDescription", {}}},
            name("Tagged"));
    install("MAPragmaProbePlain", "descriptionName", 0, {}, name("Plain"));
    install("MAPragmaProbeDuplicate", "aName:", 1, tag, [](DescriptionRequest &, auto d) {
        d->properties["label"] = Value(text(*d) + "S");
        return own(d);
    });
    install("MAPragmaProbeContainer", "ownContainer", 0, {{"magritteContainer", {}}},
            [](DescriptionRequest &r, auto) {
                auto d = field(r, "", "Own", 0);
                d->className = "MAContainer";
                return own(d);
            });
}
Graph graphOf(const DescriptionCopy &copy) {
    Graph g;
    g.format = "magritte.graph/4";
    g.root = copy.root->id;
    for (auto d : copy.retained)
        g.descriptions[d->id] = d;
    return g;
}
} // namespace
int main(int argc, char **argv) {
    try {
        setup();
        PragmaBuilder builder;
        std::ifstream in(std::string(FIXTURES) + "/pragma-scenarios.json");
        J fixtures;
        in >> fixtures;
        for (const auto &row : fixtures.at("cases")) {
            auto p = std::make_shared<Probe>(row.at("mode"));
            DescriptionTarget target(p, row.at("modelClass"));
            auto cache = std::make_shared<DynamicDescriptionCache>("scenario");
            DescriptionCopy result;
            bool success = true;
            {
                DescriptionCacheScope scope(cache);
                try {
                    result = builder.forTarget(target, "ignored");
                } catch (const Error &) {
                    success = false;
                }
                check(success == row.at("success"), row.at("mode").get<std::string>() + " success");
                auto events = p->events;
                auto count = p->containers;
                if (cache->size()) {
                    result = builder.forTarget(target, "ignored");
                    check(events == p->events && count == p->containers,
                          "Cached partial result reran provider");
                    auto hit = builder.forTarget(target, "ignored");
                    check(hit.root == result.root);
                }
                check(cache->size() == (p->mode == "container-error" ? 0 : 1));
            }
            check(!DescribedObjectScope::current());
            J actual = J::array();
            if (result.root) {
                check(text(*result.root) == row.at("label"));
                for (auto d : result.root->orderedChildren()) {
                    J item = {{"key", d->key},
                              {"label", text(*d)},
                              {"selector", nullptr},
                              {"methodClass", nullptr}};
                    if (auto c = d->definingContext) {
                        check(c->modelClass == target.className);
                        item["selector"] = *c->methodSelector;
                        item["methodClass"] = *c->methodClass();
                    }
                    actual.push_back(item);
                }
                if (p->mode == "recursive")
                    check(result.root->children.back()->referenceDescription() == result.root);
            } else
                check(row.at("label").is_null());
            check(actual == row.at("fields"), p->mode + " fields " + actual.dump());
            if (p->mode == "replacement" || p->mode == "nil-first")
                check(std::find(p->events.begin(), p->events.end(), "z:Name") != p->events.end(),
                      "Extensions must receive original");
            if (p->mode == "simple") {
                auto g = graphOf(result);
                AdaptiveModel person;
                person.id = "sample";
                person.description = result.root;
                for (auto d : result.root->children)
                    person.values[d->id] = p->readUsing(*d);
                check(validate(*result.root, person).empty());
                check(writeString(*result.root, person) == "7-Ada");
                g.objects.emplace(person.id, person);
                auto round = Graph::decode(g.encode());
                check(round.descriptions.at(g.root)->children[0]->definingContext != nullptr);
                g.format = "magritte.graph/3";
                fails([&] { g.encode(); });
                if (argc > 1)
                    std::ofstream(argv[1]) << round.encode();
            }
        }
        // No cache leaks across calls or target identities, and dynamic scopes restore on errors.
        auto p = std::make_shared<Probe>();
        DescriptionTarget t(p, "MAPragmaProbeBase");
        auto first = builder.forTarget(t, "first"), second = builder.forTarget(t, "second");
        check(first.root != second.root && p->containers == 2);
        check(DynamicDescriptionCache::current() != DynamicDescriptionCache::current());
        auto outer = std::make_shared<DynamicDescriptionCache>("outer"),
             inner = std::make_shared<DynamicDescriptionCache>("inner");
        {
            DescriptionCacheScope scope(outer);
            DescribedObjectScope object(t);
            fails([&] {
                DescriptionCacheScope nested(inner);
                throw Error("restore");
            });
            check(DynamicDescriptionCache::current() == outer &&
                  DescribedObjectScope::current()->identity == p.get());
            Probe other;
            builder.forTarget(DescriptionTarget(other, t.className), "ignored");
            check(outer->size() == 1);
        }
        // Metadata resolves current dispatch, copies share it, wire snapshots validate strict
        // types.
        auto c = std::make_shared<DescriptionContext>();
        c->modelClass = t.className;
        c->methodSelector = "descriptionName";
        DescriptionContext equal = *c;
        check(*c == equal && c->hash() == equal.hash());
        check(c->methodClass() == t.className);
        DescriptionContext empty;
        fails([&] { empty.compiledMethod(); });
        empty.modelClass = "Object";
        empty.methodSelector = "missing";
        check(!empty.compiledMethod());
        first.root->definingContext = c;
        auto copied = copyDescription(first.root, "copied");
        check(copied.root->definingContext == c);
        auto g = graphOf(first);
        auto j = J::parse(g.encode());
        // Locate a context-bearing node independent of sorted identity order.
        for (auto &node : j["descriptions"])
            if (node.contains("definingContext")) {
                node["definingContext"]["extra"] = true;
                break;
            }
        fails([&] { Graph::decode(j.dump()); });
        j = J::parse(g.encode());
        j["format"] = "magritte.graph/3";
        fails([&] { Graph::decode(j.dump()); });
        for (const auto &badValue :
             std::vector<J>{J{{"type", "string"}, {"value", "Object"}},
                            J{{"type", "class"}, {"value", "UnknownPragmaClass"}}}) {
            j = J::parse(g.encode());
            j["descriptions"][0]["definingContext"]["modelClass"] = badValue;
            fails([&] { Graph::decode(j.dump()); });
        }
        j = J::parse(g.encode());
        j["descriptions"][0]["definingContext"]["methodSelector"] =
            J{{"type", "string"}, {"value", "descriptionName"}};
        fails([&] { Graph::decode(j.dump()); });
        first.root->definingContext = std::make_shared<DescriptionContext>();
        check(Graph::decode(graphOf(first).encode())
                  .descriptions.at(first.root->id)
                  ->definingContext->modelClass == std::nullopt);
        first.root->definingContext = c;

        auto &registry = DescriptionMethodRegistry::global();
        auto original = *registry.lookup(t.className, "descriptionName");
        auto changed = original;
        changed.operation = [](DescriptionRequest &r, auto) {
            return own(field(r, "name", "Changed", 20));
        };
        {
            DescriptionCacheScope scope(outer);
            auto old = builder.forTarget(t, "ignored");
            registry.install(changed);
            check(builder.forTarget(t, "ignored").root == old.root);
            check(text(*builder
                            .forTarget(DescriptionTarget(*p, "MAPragmaProbePlain", &second),
                                       "ignored")
                            .root->orderedChildren()[1]) == "PlainAZ");
        }
        check(text(*builder.forTarget(t, "changed").root->orderedChildren()[1]) == "ChangedAZ");
        registry.install(original);
        auto tagged = *registry.lookup("MAPragmaProbeTagged", "descriptionName");
        registry.remove(tagged.className, tagged.selector);
        check(c->compiledMethod()->className == t.className);
        check(registry.lookup(tagged.className, tagged.selector)->className == t.className);
        registry.install(tagged);
        // Recursive providers are bounded before caching; retained cache handles own later graph
        // growth.
        Limits low;
        low.depth = 3;
        PragmaBuilder bounded(low);
        Probe recurse("container-recursive");
        fails([&] { bounded.forTarget(DescriptionTarget(recurse, t.className), "bounded"); });
        Limits work;
        work.visits = 2;
        PragmaBuilder little(work);
        fails([&] { little.forTarget(t, "work"); });
        DescriptionCopy early;
        std::weak_ptr<Description> weak;
        {
            auto cache = std::make_shared<DynamicDescriptionCache>("late");
            DescriptionCacheScope scope(cache);
            early = builder.forTarget(t, "ignored");
            auto q = std::make_shared<Probe>();
            auto later = builder.forTarget(DescriptionTarget(q, t.className), "ignored");
            weak = later.root;
            auto relation = std::make_shared<Description>();
            relation->id = "lateRelation";
            relation->className = "MAToOneRelationDescription";
            relation->reference = later.root;
            early.root->children.push_back(relation);
        }
        check(!weak.expired() &&
              early.root->children.back()->referenceDescription() == weak.lock());
        early = {};
        check(weak.expired());
        std::shared_ptr<DynamicDescriptionCache> threadCache;
        {
            DescriptionCacheScope scope(outer);
            std::thread isolated([&] {
                threadCache = DynamicDescriptionCache::current();
                check(!DescribedObjectScope::current());
            });
            isolated.join();
            check(threadCache != outer);
        }
        auto wrong = *registry.lookup(t.className, "descriptionName");
        wrong.arguments = 1;
        registry.install(wrong);
        fails([&] { builder.forTarget(t, "arity"); });
        registry.install(original);
        auto invalid = original;
        invalid.pragmas = {{"magritteDescription:", {}}};
        fails([&] { registry.install(invalid); });
        for (const auto &limit : std::vector<std::string>{"nodes", "items"}) {
            Limits small;
            if (limit == "nodes")
                small.nodes = 1;
            else
                small.items = 1;
            PragmaBuilder limited(small);
            fails([&] { limited.forTarget(t, "limited"); });
        }
        registry.install({"MAPragmaProbeBase",
                          "descriptionPeer",
                          0,
                          {{"magritteDescription", {}}},
                          [](DescriptionRequest &r, auto) {
                              auto &p = dynamic_cast<Probe &>(*r.target.model);
                              if (!p.peer)
                                  return DescriptionCopy{};
                              auto other = r.builder.forTarget(
                                  DescriptionTarget(*p.peer, r.target.className), "ignored");
                              auto d = field(r, "peer", "Peer", 30);
                              d->className = "MAToOneRelationDescription";
                              d->reference = other.root;
                              return DescriptionCopy{d, {other.root}, other.lifetime};
                          }});
        {
            auto cache = std::make_shared<DynamicDescriptionCache>("mutual");
            DescriptionCacheScope scope(cache);
            Probe a, b;
            a.peer = &b;
            b.peer = &a;
            auto result = builder.forTarget(DescriptionTarget(a, t.className), "ignored");
            auto peer = result.root->orderedChildren().back()->referenceDescription();
            check(peer->orderedChildren().back()->referenceDescription() == result.root);
            check(a.containers == 1 && b.containers == 1 && cache->size() == 2);
        }
        registry.remove(t.className, "descriptionPeer");
        {
            DescriptionCacheScope scope(std::make_shared<DynamicDescriptionCache>("identity"));
            Probe a, b;
            int token;
            auto first = builder.forTarget(DescriptionTarget(a, t.className, &token), "ignored");
            auto second = builder.forTarget(DescriptionTarget(b, t.className, &token), "ignored");
            check(first.root == second.root && a.containers == 1 && b.containers == 0);
        }
        std::cout << checks << " pragma checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
