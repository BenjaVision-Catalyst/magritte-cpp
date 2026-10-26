#include "detail.hpp"
#include <algorithm>
#include <mutex>
#include <set>
namespace magritte {
namespace {
thread_local std::shared_ptr<DynamicDescriptionCache> activeCache;
thread_local std::optional<DescriptionTarget> activeDescribed;
void identity(const std::string &n) {
    if (n.empty() ||
        !std::all_of(n.begin(), n.end(), [](unsigned char c) { return c >= 33 && c <= 126; }))
        throw Error("Invalid description method identity");
}
} // namespace
struct DescriptionMethodRegistry::State {
    mutable std::mutex mutex;
    std::map<std::string, std::vector<DescriptionMethod>> methods;
};
DescriptionMethodRegistry::DescriptionMethodRegistry() : state_(std::make_unique<State>()) {
    DescriptionMethod method;
    method.className = "Object";
    method.selector = "descriptionContainer";
    method.pragmas = {{"magritteContainer", {}}};
    method.operation = [](DescriptionRequest &request, std::shared_ptr<Description>) {
        auto d = std::make_shared<Description>();
        d->id = request.builder.freshId();
        d->className = "MAPriorityContainer";
        auto info = ClassRegistry::global().find(request.target.className);
        if (!info)
            throw Error("Unknown model class");
        d->properties["label"] = Value(info->label);
        return DescriptionCopy{d, {d}, {}};
    };
    state_->methods["Object"].push_back(std::move(method));
}
DescriptionMethodRegistry::~DescriptionMethodRegistry() = default;
DescriptionMethodRegistry &DescriptionMethodRegistry::global() {
    static DescriptionMethodRegistry registry;
    return registry;
}
void DescriptionMethodRegistry::install(DescriptionMethod method) {
    identity(method.className);
    identity(method.selector);
    if (!ClassRegistry::global().find(method.className) || !method.operation)
        throw Error("Unregistered description method class or empty operation");
    for (const auto &pragma : method.pragmas) {
        identity(pragma.selector);
        if (std::count(pragma.selector.begin(), pragma.selector.end(), ':') !=
            std::ptrdiff_t(pragma.arguments.size()))
            throw Error("Pragma arity mismatch");
        for (const auto &arg : pragma.arguments)
            (void)encodeValue(arg);
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto &methods = state_->methods[method.className];
    auto old = std::find_if(methods.begin(), methods.end(),
                            [&](const auto &m) { return m.selector == method.selector; });
    if (old == methods.end())
        methods.push_back(std::move(method));
    else
        *old = std::move(method);
}
void DescriptionMethodRegistry::remove(const std::string &cls, const std::string &selector) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    auto &v = state_->methods[cls];
    v.erase(
        std::remove_if(v.begin(), v.end(), [&](const auto &m) { return m.selector == selector; }),
        v.end());
}
std::optional<DescriptionMethod>
DescriptionMethodRegistry::lookup(const std::string &cls, const std::string &selector) const {
    std::string name = cls;
    while (!name.empty()) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto found = state_->methods.find(name);
            if (found != state_->methods.end())
                for (const auto &method : found->second)
                    if (method.selector == selector)
                        return method;
        }
        auto info = ClassRegistry::global().find(name);
        if (!info)
            throw Error("Unknown method lookup class");
        name = info->superclass;
    }
    return {};
}
std::vector<DescriptionMethod>
DescriptionMethodRegistry::annotated(const std::string &cls, const std::string &pragma) const {
    std::vector<DescriptionMethod> result;
    std::string name = cls;
    while (!name.empty()) {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto found = state_->methods.find(name);
            if (found != state_->methods.end())
                for (const auto &method : found->second)
                    for (const auto &p : method.pragmas)
                        if (p.selector == pragma) {
                            auto tagged = method;
                            tagged.pragmas = {p};
                            result.push_back(std::move(tagged));
                        }
        }
        if (name == "Object")
            break;
        auto info = ClassRegistry::global().find(name);
        if (!info)
            throw Error("Unknown pragma lookup class");
        name = info->superclass;
    }
    return result;
}
bool DescriptionContext::operator==(const DescriptionContext &c) const {
    return modelClass == c.modelClass && methodSelector == c.methodSelector;
}
std::size_t DescriptionContext::hash() const {
    return std::hash<std::string>{}(modelClass.value_or("")) ^
           std::hash<std::string>{}(methodSelector.value_or(""));
}
std::optional<DescriptionMethod> DescriptionContext::compiledMethod() const {
    if (!modelClass)
        throw Error("Description context has no model class");
    if (!methodSelector)
        return {};
    return DescriptionMethodRegistry::global().lookup(*modelClass, *methodSelector);
}
std::optional<std::string> DescriptionContext::methodClass() const {
    auto method = compiledMethod();
    return method ? std::optional<std::string>(method->className) : std::nullopt;
}
struct DynamicDescriptionCache::State {
    std::string prefix;
    Limits limits;
    std::size_t serial = 0, work = 0, depth = 0;
    struct Entry {
        DescriptionTarget target;
        std::shared_ptr<Description> root;
    };
    std::map<const void *, Entry> entries;
    std::map<std::string, std::shared_ptr<Description>> owners;
    std::vector<std::shared_ptr<void>> lifetimes;
    void tick(std::size_t d = 0) {
        if (++work > limits.visits || d > limits.depth)
            throw Error("Pragma discovery work/depth limit");
    }
    void retain(const DescriptionCopy &result, const void *self) {
        if (result.lifetime && result.lifetime.get() != self)
            lifetimes.push_back(result.lifetime);
        std::vector<std::pair<std::shared_ptr<Description>, std::size_t>> pending;
        for (const auto &d : result.retained)
            pending.push_back({d, 0});
        if (result.root)
            pending.push_back({result.root, 0});
        std::set<const Description *> seen;
        while (!pending.empty()) {
            auto pair = pending.back();
            pending.pop_back();
            auto d = pair.first;
            tick(pair.second);
            if (!d)
                throw Error("Null discovery owner");
            if (!seen.insert(d.get()).second)
                continue;
            auto found = owners.find(d->id);
            if (found != owners.end() && found->second != d)
                throw Error("Discovery description identity collision");
            if (found == owners.end()) {
                if (owners.size() >= limits.nodes)
                    throw Error("Discovery owner limit");
                identity(d->id);
                owners[d->id] = d;
            }
            if (d->children.size() > limits.items)
                throw Error("Discovery item limit");
            for (const auto &child : d->children)
                pending.push_back({child, pair.second + 1});
            if (d->isReference())
                if (auto ref = detail::storedReference(*d)) pending.push_back({ref, pair.second + 1});
        }
    }
    DescriptionCopy result(std::shared_ptr<Description> root, std::shared_ptr<void> self) {
        DescriptionCopy out;
        out.root = std::move(root);
        out.lifetime = std::move(self);
        for (const auto &p : owners)
            out.retained.push_back(p.second);
        return out;
    }
};
DynamicDescriptionCache::DynamicDescriptionCache(std::string prefix, Limits limits)
    : state_(std::make_unique<State>()) {
    identity(prefix);
    state_->prefix = std::move(prefix);
    state_->limits = limits;
}
DynamicDescriptionCache::~DynamicDescriptionCache() = default;
std::shared_ptr<DynamicDescriptionCache> DynamicDescriptionCache::current() {
    return activeCache ? activeCache : std::make_shared<DynamicDescriptionCache>();
}
std::size_t DynamicDescriptionCache::size() const { return state_->entries.size(); }
DescriptionCacheScope::DescriptionCacheScope(std::shared_ptr<DynamicDescriptionCache> c)
    : previous_(activeCache) {
    if (!c)
        throw Error("Null description cache");
    activeCache = std::move(c);
}
DescriptionCacheScope::~DescriptionCacheScope() { activeCache = previous_; }
DescribedObjectScope::DescribedObjectScope(DescriptionTarget target) : previous_(activeDescribed) {
    activeDescribed = std::move(target);
}
DescribedObjectScope::~DescribedObjectScope() { activeDescribed = previous_; }
std::optional<DescriptionTarget> DescribedObjectScope::current() { return activeDescribed; }
std::string PragmaBuilder::freshId() {
    if (!activeCache)
        throw Error("No dynamic description cache");
    auto &state = *activeCache->state_;
    state.tick();
    return state.prefix + "." + std::to_string(++state.serial);
}
DescriptionCopy PragmaBuilder::invoke(DescriptionTarget target, const std::string &selector,
                                      std::shared_ptr<Description> argument, std::size_t count) {
    auto cache = activeCache;
    if (!cache)
        throw Error("No dynamic description cache");
    cache->state_->tick(cache->state_->depth);
    auto method = DescriptionMethodRegistry::global().lookup(target.className, selector);
    if (!method || method->arguments != count)
        throw Error("Description method dispatch/arity failure: " + selector);
    DescriptionRequest request{std::move(target), *this};
    auto result = method->operation(request, std::move(argument));
    cache->state_->retain(result, cache.get());
    return result;
}
DescriptionCopy PragmaBuilder::forTarget(DescriptionTarget target, const std::string &prefix) {
    if (!target.model || !target.identity ||
        !ClassRegistry::global().includesBehavior(target.className, "Object"))
        throw Error("Invalid described target");
    auto cache =
        activeCache ? activeCache : std::make_shared<DynamicDescriptionCache>(prefix, limits_);
    DescriptionCacheScope scope(cache);
    auto &state = *cache->state_;
    state.tick(state.depth);
    auto hit = state.entries.find(target.identity);
    if (hit != state.entries.end())
        return state.result(hit->second.root, cache);
    if (state.depth >= state.limits.depth)
        throw Error("Pragma recursive container depth limit");
    ++state.depth;
    struct Leave {
        DynamicDescriptionCache::State &state;
        ~Leave() { --state.depth; }
    } leave{state};
    auto &registry = DescriptionMethodRegistry::global();
    auto containers = registry.annotated(target.className, containerPragma());
    if (containers.size() > state.limits.items)
        throw Error("Container pragma item limit");
    auto getter = std::find_if(containers.begin(), containers.end(),
                               [](const auto &method) { return method.arguments == 0; });
    if (getter == containers.end())
        throw Error("No container pragma getter");
    std::shared_ptr<Description> root;
    {
        DescribedObjectScope described(target);
        root = invoke(target, getter->selector, {}, 0).root;
        for (const auto &method : containers)
            if (method.arguments == 1)
                invoke(target, method.selector, root, 1);
    }
    // Store the completed container before any field getters, but not before its own
    // providers/extensions.
    if (state.entries.size() >= state.limits.nodes)
        throw Error("Description cache target limit");
    state.entries.emplace(target.identity, DynamicDescriptionCache::State::Entry{target, root});
    auto declarations = registry.annotated(target.className, descriptionPragma()),
         extensions = registry.annotated(target.className, extensionPragma());
    if (declarations.size() > state.limits.items || extensions.size() > state.limits.items)
        throw Error("Description pragma item limit");
    std::set<std::string> selectors;
    for (const auto &method : declarations)
        selectors.insert(method.selector);
    std::map<std::string, std::vector<std::string>> extensionSelectors;
    for (const auto &method : extensions)
        for (const auto &pragma : method.pragmas)
            if (pragma.selector == extensionPragma() && !pragma.arguments.empty()) {
                const auto &value = pragma.arguments[0];
                std::string targetName;
                if (auto s = std::get_if<Value::Symbol>(&value.data))
                    targetName = s->utf8;
                else if (auto s = std::get_if<std::string>(&value.data))
                    targetName = *s;
                else
                    continue;
                extensionSelectors[targetName].push_back(method.selector);
            }
    for (auto &item : extensionSelectors)
        std::sort(item.second.begin(), item.second.end());
    std::vector<std::shared_ptr<Description>> fields;
    for (const auto &selector : selectors) {
        auto initial = invoke(target, selector, {}, 0).root;
        if (!initial)
            continue;
        initial->definingContext = std::make_shared<DescriptionContext>();
        initial->definingContext->modelClass = target.className;
        initial->definingContext->methodSelector = selector;
        auto result = initial;
        // Upstream passes the original description to every extension, not the accumulator.
        for (const auto &extension : extensionSelectors[selector])
            result = invoke(target, extension, initial, 1).root;
        if (result)
            fields.push_back(result);
    }
    if (!root || !root->isContainer())
        throw Error("Container does not understand addAll:");
    if (root->children.size() + fields.size() > state.limits.items)
        throw Error("Discovered container item limit");
    root->addAll(fields, state.limits);
    state.retain({root, {}, {}}, cache.get());
    return state.result(root, cache);
}
} // namespace magritte
