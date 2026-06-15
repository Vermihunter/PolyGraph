//
// Created by akos on 8. 3. 2026.
//

#ifndef CPGDB_DATAMODELTOGPUGRAPHCONVERTER_HPP
#define CPGDB_DATAMODELTOGPUGRAPHCONVERTER_HPP

#include <data_model/Record.hpp>
#include <gpu/GPU_Graph.hpp>

struct DataModelToGPUGraphConverter {
    template <typename v_t ,typename e_t, typename w_t, typename r_t, bool s_t, bool m_gpu>
    gpu_graph_t<v_t, e_t, w_t, r_t, s_t, m_gpu> convert(raft::handle_t& handle, const std::vector<record_t>& data) {
        std::vector<v_t> h_src;
        std::vector<v_t> h_dst;

        // Mapping from string EntityId to vertex_t (int)
        std::unordered_map<std::string, v_t> id_map;
        v_t next_id = 0;

        // Lambda to handle string-to-int mapping
        auto get_or_create_id = [&](const std::string& str_id) -> v_t {
            auto it = id_map.find(str_id);
            if (it != id_map.end()) {
                return it->second;
            }
            id_map[str_id] = next_id;
            return next_id++;
        };

        // 1. Traverse and Map
        for (const auto& record : data) {
            if (!record) continue;
            for (const auto& val : record->fields) {
                if (val.template is<Edge>()) {
                    const auto& edge = val.template as<Edge>();

                    // Convert string IDs to integer v_t
                    v_t s = get_or_create_id(edge.src);
                    v_t d = get_or_create_id(edge.dst);

                    h_src.push_back(s);
                    h_dst.push_back(d);
                } else if (val.template is<Node>()) {
                    // Ensure nodes with no edges are still registered
                    get_or_create_id(val.template as<Node>().id);
                }
            }
        }

        // 2. Prepare Vertex List [0, 1, 2, ... next_id-1]
        std::vector<v_t> h_vertices(next_id);
        std::iota(h_vertices.begin(), h_vertices.end(), 0);

        auto stream = handle.get_stream();

        // 3. Move to GPU
        rmm::device_uvector<v_t> d_src(h_src.size(), stream);
        rmm::device_uvector<v_t> d_dst(h_dst.size(), stream);
        rmm::device_uvector<v_t> d_vertices(h_vertices.size(), stream);

        raft::update_device(d_src.data(), h_src.data(), h_src.size(), stream);
        raft::update_device(d_dst.data(), h_dst.data(), h_dst.size(), stream);
        raft::update_device(d_vertices.data(), h_vertices.data(), h_vertices.size(), stream);

        // 4. Graph Properties
        cugraph::graph_properties_t graph_props{false, true};
        std::vector<cugraph::arithmetic_device_uvector_t> edge_props;


        std::vector<std::string> id_to_string(id_map.size());
        for (auto const& [name, id] : id_map) {
            id_to_string[id] = name;
        }

        // 5. Create via Factory
        auto graph =  GPU_Graph<v_t, e_t, w_t, r_t, s_t, m_gpu>::from_edge_list(
            handle,
            std::move(d_src),      // Explicitly move
            std::move(d_dst),      // Explicitly move
            std::move(d_vertices), // Explicitly move
            graph_props,
            std::move(edge_props)
        );

        graph->id_to_string_ = std::move(id_to_string);

        return graph;
    }

};

#endif //CPGDB_DATAMODELTOGPUGRAPHCONVERTER_HPP