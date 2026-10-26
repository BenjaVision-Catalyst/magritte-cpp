#include "detail.hpp"
#include <algorithm>
#include <set>
namespace magritte {
namespace {
const std::vector<std::string> concrete{"MAContainer",
                                        "MAPriorityContainer",
                                        "MAStringDescription",
                                        "MASymbolDescription",
                                        "MAMemoDescription",
                                        "MAPasswordDescription",
                                        "MATokenDescription",
                                        "MANumberDescription",
                                        "MABooleanDescription",
                                        "MAToOneRelationDescription",
                                        "MAToManyRelationDescription",
                                        "MAToManyScalarRelationDescription",
                                        "MASingleOptionDescription",
                                        "MAMultipleOptionDescription"};
Graph expanded(const Description &target, Limits limits) {
    auto graph = descriptionModelFor(target, limits);
    auto root = graph.descriptions.at(graph.root);
    auto add = [&](std::string key, std::string cls, std::int64_t priority) {
        auto d = std::make_shared<Description>();
        d->id = graph.root + "." + key;
        d->className = cls;
        Accessor a;
        a.className = "MASelectorAccessor";
        a.name = key;
        a.writeName = key == "children" ? "setChildren:" : key + ":";
        d->accessor = a;
        d->properties["priority"] = Value(priority);
        root->children.push_back(d);
        graph.descriptions.emplace(d->id, d);
        if (d->isReference()) {
            auto ref = std::make_shared<Description>();
            ref->id = d->id + ".reference";
            Accessor null;
            null.className = "MANullAccessor";
            ref->accessor = null;
            d->reference = ref;
            graph.descriptions.emplace(ref->id, ref);
        }
        return d;
    };
    add("name", "MASymbolDescription", 1);
    if (target.isContainer()) {
        auto d = add("children", "MAToManyRelationDescription", 400);
        d->properties["label"] = Value("Elements");
        d->properties["ordered"] = Value(true);
        d->properties["default"] = Value(Value::Collection{"ordered", {}});
        for (const auto &cls : concrete)
            if (cls != "MAContainer" && cls != "MAPriorityContainer")
                d->classes.push_back(cls);
    }
    if (target.isReference()) {
        auto d = add("reference", "MAToOneRelationDescription", 400);
        d->properties["label"] = Value("Description");
        if (target.isRelation()) {
            d->classes = {"MAContainer", "MAPriorityContainer"};
            d->properties["required"] = Value(false);
        } else {
            d->classes = concrete;
            d->properties["required"] = Value(true);
        }
    }
    if (target.isOption()) {
        auto d = add("optionsTextual", "MAMemoDescription", 410);
        d->properties["label"] = Value("Options");
        d->properties["default"] = Value(Value::Collection{"ordered", {}});
    }
    // Root insertion order follows the explicitly selected upstream methods.
    return graph;
}
} // namespace
struct RecursiveDescriptionModel::State : std::enable_shared_from_this<State> {
    Graph graph;
    Limits limits;
    std::size_t generation = 0;
    std::set<std::string> metaIds;
    struct Binding : Model {
        std::weak_ptr<State> owner;
        std::shared_ptr<Description> target;
        Binding(std::shared_ptr<State> s, std::shared_ptr<Description> d) : owner(s), target(std::move(d)) {}
        std::shared_ptr<State> state() const {
            auto s = owner.lock();
            if (!s)
                throw Error("Recursive metadata session expired");
            return s;
        }
        bool kindOf(const std::string &name) const override { return target->kindOf(name); }
        Value ref(const std::shared_ptr<Description> &d) const {
            auto s = state();
            if (!s->graph.nativeObjects.count(d->id) || s->graph.descriptions.at(d->id) != d)
                s->bind(d);
            return Value(Value::Reference{d->id});
        }
        std::shared_ptr<Description> description(const Value &v) const {
            auto s = state();
            auto &model = s->graph.resolve(v);
            auto b = dynamic_cast<Binding *>(&model);
            if (!b)
                throw Error("Metadata reference must identify a description");
            return b->target;
        }
        Value readSlot(const std::string &strategy, const std::string &name) const override {
            auto s = state();
            if (strategy != "MASelectorAccessor")
                return target->readSlot(strategy, name);
            if (name == "children" && target->isContainer()) {
                Value::Array values;
                for (const auto &d : target->orderedChildren())
                    values.push_back(ref(d));
                return Value(Value::Collection{"ordered", values});
            }
            if (name == "reference" && target->isReference())
                return ref(target->referenceDescription());
            if (name == "name") {
                auto i = target->properties.find("name");
                if (i != target->properties.end())
                    return i->second;
                auto a = target->effectiveAccessor();
                if (a.className == "MAIdentityAccessor" || a.className == "MANullAccessor")
                    return {};
                return a.className != "MAVariableAccessor" ? Value(Value::Symbol{a.name}) : Value(a.name);
            }
            if (name == "optionsTextual" && target->isOption()) {
                std::size_t work = 0;
                (void)detail::valueKey(Value(target->options), s->limits, work);
                std::string result;
                bool first = true;
                for (const auto &v : target->options) {
                    if (!first)
                        result += '\r';
                    first = false;
                    result += writeString(*target->referenceDescription(), v, &s->graph, s->limits);
                }
                return Value(result);
            }
            return target->readSlot(strategy, name);
        }
        void writeSlot(const std::string &strategy, const std::string &name, Value v) override {
            auto s = state();
            if (strategy != "MASelectorAccessor") {
                target->writeSlot(strategy, name, std::move(v));
                return;
            }
            if (name == "setChildren:" && target->isContainer()) {
                if (!v.isCollection() || v.collectionSize() > s->limits.items)
                    throw Error("Children require a bounded collection");
                std::vector<std::shared_ptr<Description>> next;
                for (const auto &e : v.elements())
                    next.push_back(description(e));
                auto previous = target->children;
                target->children = std::move(next);
                try {
                    visit(*target, [](const auto &) {}, s->limits);
                } catch (...) {
                    target->children = std::move(previous);
                    throw;
                }
                return;
            }
            if (name == "reference:" && target->isReference()) {
                target->reference = description(v);
                return;
            }
            if (name == "name:") {
                if (!v.isNil() && v.data.index() != 3 && v.data.index() != 8)
                    throw Error("Name requires text or Symbol");
                target->properties["name"] = std::move(v);
                return;
            }
            if (name == "optionsTextual:" && target->isOption()) {
                std::string text;
                if (!v.isNil()) {
                    auto p = std::get_if<std::string>(&v.data);
                    if (!p)
                        throw Error("Options text requires String");
                    text = *p;
                }
                Value::Array values;
                std::size_t start = 0;
                while (start < text.size()) {
                    auto end = text.find_first_of("\r\n", start);
                    if (end == std::string::npos)
                        end = text.size();
                    values.push_back(readString(*target->referenceDescription(),
                                                text.substr(start, end - start), s->limits));
                    if (values.size() > s->limits.items)
                        throw Error("Options item limit");
                    start = end + 1;
                    if (end < text.size() && text[end] == '\r' && start < text.size() && text[start] == '\n')
                        ++start;
                }
                target->options = std::move(values);
                return;
            }
            target->writeSlot(strategy, name, std::move(v));
        }
        Value readUsing(const Description &d) const override {
            auto s = state();
            auto value = d.effectiveAccessor().read(*this, &s->graph, s->limits);
            (void)encodeValue(value, s->limits);
            return value;
        }
        void writeUsing(const Description &d, Value v) override {
            auto s = state();
            (void)encodeValue(v, s->limits);
            d.effectiveAccessor().write(*this, std::move(v), &s->graph, s->limits);
        }
    };
    explicit State(Limits l) : limits(l) {}
    std::shared_ptr<Model> bind(std::shared_ptr<Description> target) {
        if (!target)
            throw Error("Null metadata target");
        std::map<std::string, std::shared_ptr<Description>> pending;
        visit(*target, [&](const Description &d) { detail::checkDescription(d); }, limits);
        std::set<const Description *> seen;
        std::function<void(std::shared_ptr<Description>)> walk = [&](auto d) {
            if (!seen.insert(d.get()).second)
                return;
            if (d->id.empty() ||
                !std::all_of(d->id.begin(), d->id.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
                throw Error("Invalid metadata target identity");
            if ((pending.count(d->id) && pending.at(d->id) != d) ||
                (graph.descriptions.count(d->id) && graph.descriptions.at(d->id) != d))
                throw Error("Metadata target identity already bound");
            pending.emplace(d->id, d);
            for (const auto &c : d->children)
                walk(c);
            if (d->isReference())
                walk(d->referenceDescription());
        };
        walk(target);
        std::size_t added = 0;
        for (const auto &p : pending)
            if (!graph.descriptions.count(p.first))
                ++added;
        if (graph.descriptions.size() + added > limits.nodes)
            throw Error("Recursive metadata node limit");
        for (const auto &p : pending) {
            graph.descriptions.emplace(p);
            if (!graph.nativeObjects.count(p.first))
                graph.nativeObjects.emplace(p.first, std::make_shared<Binding>(shared_from_this(), p.second));
        }
        return graph.nativeObjects.at(target->id);
    }
};
RecursiveDescriptionModel::RecursiveDescriptionModel(Limits l) : state_(std::make_shared<State>(l)) {}
Graph &RecursiveDescriptionModel::resolver() { return state_->graph; }
std::shared_ptr<Model> RecursiveDescriptionModel::bind(std::shared_ptr<Description> d) {
    return state_->bind(std::move(d));
}
std::shared_ptr<Description> RecursiveDescriptionModel::describe(std::shared_ptr<Description> target) {
    state_->bind(target);
    auto next = expanded(*target, state_->limits);
    auto root = next.descriptions.at(next.root);
    auto old = next.root;
    auto prefix = "recursive." + std::to_string(state_->generation + 1) + "." + target->id;
    std::map<std::string, std::shared_ptr<Description>> renamed;
    for (auto &p : next.descriptions) {
        p.second->id = prefix + p.first.substr(old.size());
        renamed.emplace(p.second->id, p.second);
    }
    // Preflight all identities and aggregate capacity before publishing a level.
    if (state_->graph.descriptions.size() + renamed.size() > state_->limits.nodes)
        throw Error("Recursive metadata node limit");
    for (const auto &p : renamed)
        if (state_->graph.descriptions.count(p.first))
            throw Error("Generated metadata identity collision");
    state_->bind(root);
    for (const auto &p : renamed)
        state_->metaIds.insert(p.first);
    ++state_->generation;
    state_->graph.root = root->id;
    return root;
}
Graph RecursiveDescriptionModel::schemas() const {
    Graph result;
    result.root = state_->graph.root;
    for (const auto &id : state_->metaIds)
        result.descriptions.emplace(id, state_->graph.descriptions.at(id));
    return result;
}
} // namespace magritte
