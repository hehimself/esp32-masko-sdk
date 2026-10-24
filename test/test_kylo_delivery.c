#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "masko_graph_json.h"

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    if (fseek(f, 0, SEEK_END) || ftell(f) <= 0) return 2;
    const size_t n = (size_t)ftell(f);
    rewind(f);
    char *text = malloc(n);
    if (!text || fread(text, 1, n, f) != n) return 2;
    fclose(f);
    masko_graph_json_t parsed;
    const int rc = masko_graph_json_parse(text, n, &parsed);
    free(text);
    if (rc != MASKO_GRAPH_OK) {
        fprintf(stderr, "delivery parse failed: %s\n", masko_graph_strerror(rc));
        masko_graph_json_free(&parsed);
        return 1;
    }
    masko_graph_t graph;
    if (masko_graph_json_bind(&parsed, &graph) != MASKO_GRAPH_OK) return 1;
    int urls = 0, empty = 0, reverse = 0, gt = 0;
    for (int i = 0; i < graph.nedges; i++) {
        if (graph.edges[i].clip) urls++; else empty++;
        if (parsed.edge_reverse[i]) reverse++;
    }
    for (int i = 0; i < parsed.nconds_total; i++)
        if (parsed.conds[i].op == MASKO_OP_GT) gt++;
    const bool ok = graph.nnodes == 11 && graph.nedges == 42 && urls == 25 &&
                    empty == 17 && reverse == 6 && gt == 3 &&
                    strcmp(graph.initial_node, "jar-gazing") == 0;
    printf("Kylo delivery: nodes=%d edges=%d URLs=%d empty=%d reverse=%d > conditions=%d %s\n",
           graph.nnodes, graph.nedges, urls, empty, reverse, gt,
           ok ? "PASS" : "FAIL");
    masko_graph_rt_t runtime;
    bool route_ok = masko_graph_init(&runtime, &graph, NULL, NULL, 0) == MASKO_GRAPH_OK;
    if (route_ok) {
        route_ok = masko_graph_set_bool(&runtime, "behavior::working", true) == MASKO_GRAPH_OK;
        masko_step_t step = {0};
        route_ok = route_ok && masko_graph_next(&runtime, &step) == MASKO_GRAPH_OK &&
                   step.edge && strcmp(step.edge->target, "specimen-notes") == 0;
        masko_graph_free(&runtime);
    }
    printf("Any-State working route: %s\n", route_ok ? "PASS" : "FAIL");
    masko_graph_json_free(&parsed);
    return ok && route_ok ? 0 : 1;
}
