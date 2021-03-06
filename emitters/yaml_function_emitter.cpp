/*
Copyright 2018 Adobe
All Rights Reserved.

NOTICE: Adobe permits you to use, modify, and distribute this file in
accordance with the terms of the Adobe license agreement accompanying
it. If you have received this file from a source other than Adobe,
then your use, modification, or distribution of it requires the prior
written permission of Adobe. 
*/

// identity
#include "yaml_function_emitter.hpp"

// stdc++
#include <iostream>

/**************************************************************************************************/

namespace hyde {

/**************************************************************************************************/

bool yaml_function_emitter::do_merge(const std::string& filepath,
                                     const json& have,
                                     const json& expected,
                                     json& out_merged) {
    bool failure{false};

    failure |= check_scalar(filepath, have, expected, "", out_merged, "defined-in-file");
    // failure |= check_scalar(filepath, have, expected, "", out_merged, "annotation");
    failure |= check_map(
        filepath, have, expected, "", out_merged, "overloads",
        [this](const std::string& filepath, const json& have, const json& expected,
               const std::string& nodepath, json& out_merged) {
            bool failure{false};

            failure |= check_scalar(filepath, have, expected, nodepath, out_merged, "description");
            failure |= check_scalar(filepath, have, expected, nodepath, out_merged, "signature_with_names");
            failure |= check_scalar(filepath, have, expected, nodepath, out_merged, "return");
            // failure |= check_scalar(filepath, have, expected, nodepath, out_merged,
            // "annotation");

            if (expected.count("arguments")) {
                failure |= check_object_array(
                    filepath, have, expected, nodepath, out_merged, "arguments", "name",
                    [this](const std::string& filepath, const json& have, const json& expected,
                           const std::string& nodepath, json& out_merged) {
                        bool failure{false};

                        failure |=
                            check_scalar(filepath, have, expected, nodepath, out_merged, "type");
                        failure |= check_scalar(filepath, have, expected, nodepath, out_merged,
                                                "description");

                        return failure;
                    });
            }

            return failure;
        });

    return failure;
}

/**************************************************************************************************/

bool yaml_function_emitter::emit(const json& j) {
    boost::filesystem::path dst;
    std::string name;
    std::string filename;
    std::string defined_path;
    std::size_t count{0};
    json overloads = json::object();
    bool is_ctor{false};
    bool is_dtor{false};

    for (const auto& overload : j) {
        if (!count) {
            dst = dst_path(overload);
            // always the unqualified name, as free functions may be defined
            // over different namespaces.
            name = overload["name"];
            // prefix to keep free-function from colliding with class member (e.g., `swap`)
            filename = (_as_methods ? "m_" : "f_") + filename_filter(name);
            defined_path = defined_in_file(overload["defined-in-file"], _src_root);
            if (overload.count("is_ctor") && overload["is_ctor"]) is_ctor = true;
            if (overload.count("is_dtor") && overload["is_dtor"]) is_dtor = true;
        }

        const std::string& key = static_cast<const std::string&>(overload["signature"]);
        overloads[key]["signature_with_names"] = overload["signature_with_names"];
        overloads[key]["description"] = tag_value_missing_k;
        overloads[key]["return"] = tag_value_optional_k;
        maybe_annotate(overload, overloads[key]);

        if (!overload["arguments"].empty()) {
            std::size_t count{0};
            auto& args = overloads[key]["arguments"];
            for (const auto& arg : overload["arguments"]) {
                auto& cur_arg = args[count];
                const std::string& name = arg["name"];
                const bool unnamed = name.empty();
                cur_arg["name"] = unnamed ? "unnamed-" + std::to_string(count) : name;
                cur_arg["type"] = arg["type"];
                cur_arg["description"] = tag_value_optional_k;
                if (unnamed) cur_arg["unnamed"] = true;
                ++count;
            }
        }

        ++count;
    }

    json node = base_emitter_node(_as_methods ? "method" : "function", name,
                                  _as_methods ? "method" : "function");
    node["defined-in-file"] = defined_path;
    maybe_annotate(j, node);
    node["overloads"] = std::move(overloads);
    if (is_ctor) node["is_ctor"] = true;
    if (is_dtor) node["is_dtor"] = true;

    return reconcile(std::move(node), _dst_root, dst / (filename + ".md"));
}

/**************************************************************************************************/

} // namespace hyde

/**************************************************************************************************/
