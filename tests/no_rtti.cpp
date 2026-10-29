#include <magritte/magritte.hpp>
#include <iostream>
struct User { std::string name; };
int main() {
    User value{"Ada"};
    magritte::NativeModel model(&value);
    magritte::Description field;
    field.id = "name";
    field.nativeRead = [](const void* object) { return magritte::Value(static_cast<const User*>(object)->name); };
    field.nativeWrite = [](void* object, const magritte::Value& input) { static_cast<User*>(object)->name = std::get<std::string>(input.data); };
    if (model.readUsing(field) != magritte::Value("Ada")) return 1;
    model.writeUsing(field, magritte::Value("Grace"));
    if (value.name != "Grace") return 1;
    std::cout << "Explicit native access works without RTTI\n";
}
