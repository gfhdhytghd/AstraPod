/* Validate a bounded flat PDa patch before handing it to the stock evaluator.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pdbox-patch.h"
#include <string.h>

#ifdef PLUGIN
#include "plugin.h"
#define strcmp rb->strcmp
#define strncmp rb->strncmp
#define strlen rb->strlen
#define memcpy rb->memcpy
#define memset rb->memset
#endif

#define PDBOX_OBJECTS 128
#define PDBOX_CONNECTIONS 256
#define PDBOX_RECORD_BYTES 512
#define PDBOX_RECORD_ATOMS 40
#define PDBOX_DISPATCH_LIMIT 512

struct pdbox_ports {
    unsigned char in, out;
};

struct pdbox_edge {
    unsigned char from, to;
};

struct pdbox_graph {
    struct pdbox_ports ports[PDBOX_OBJECTS];
    char names[PDBOX_OBJECTS][PDBOX_NAME_BYTES];
    bool sends[PDBOX_OBJECTS];
    struct pdbox_edge edges[PDBOX_CONNECTIONS];
    unsigned int objects, connections;
    bool receiver, dac, wiring;
};

static bool equals(const char *word, const char *expected)
{
    return strcmp(word, expected) == 0;
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

/* Use the same decimal grammar as PDa. Cap digit counts before conversion. */
static bool number(const char *word, double *output)
{
    const char *p = word;
    bool negative = *p == '-';
    p += negative;
    unsigned int digits = 0;
    double value = 0, scale = 1;
    while (digit(*p))
    {
        if (++digits > 8)
            return false;
        value = value * 10 + (*p++ - '0');
    }
    if (*p == '.')
    {
        p++;
        if (!digits && !digit(*p))
            return false;
        while (digit(*p))
        {
            if (++digits > 12)
                return false;
            scale *= 0.1;
            value += (*p++ - '0') * scale;
        }
    }
    if (!digits)
        return false;

    if (*p == 'e' || *p == 'E')
    {
        p++;
        bool small = *p == '-';
        if (small || *p == '+')
            p++;
        if (!digit(*p))
            return false;
        unsigned int exponent = *p++ - '0';
        if (digit(*p))
            exponent = exponent * 10 + (*p++ - '0');
        if (exponent > 12)
            return false;
        while (exponent--)
            value *= small ? 0.1 : 10;
    }
    if (*p || value > 1000000)
        return false;
    *output = negative ? -value : value;
    return true;
}

static bool integer(const char *word, int low, int high, int *output)
{
    double value;
    if (!number(word, &value) || value < low || value > high ||
        value != (int)value)
        return false;
    *output = (int)value;
    return true;
}

static bool numbers(char **words, unsigned int count, double bound)
{
    for (unsigned int i = 0; i < count; i++)
    {
        double value;
        if (!number(words[i], &value) || value < -bound || value > bound)
            return false;
    }
    return true;
}

static bool pod_name(const char *word)
{
    if (strncmp(word, "pod_", 4) || !word[4])
        return false;
    for (const char *p = word + 4; *p; p++)
    {
        if (!digit(*p) && !(*p >= 'a' && *p <= 'z') &&
            !(*p >= 'A' && *p <= 'Z') && *p != '_')
            return false;
    }
    return true;
}

static bool widget(struct pdbox_patch *patch, struct pdbox_widget_spec spec,
                   const char *label)
{
    size_t length = strlen(label);
    if (patch->count == PDBOX_WIDGETS || length >= sizeof(spec.name))
        return false;
    for (unsigned int i = 0; i < patch->count && spec.kind != PDBOX_TEXT; i++)
    {
        if (patch->widgets[i].kind != PDBOX_TEXT &&
            equals(label, patch->widgets[i].name))
            return false;
    }
    memcpy(spec.name, label, length + 1);
    patch->widgets[patch->count++] = spec;
    return true;
}

static bool iem_widget(const char *name, char **args, unsigned int argc,
                       struct pdbox_patch *patch, struct pdbox_widget_spec spec)
{
    const bool bang = equals(name, "bng");
    const bool slider = equals(name, "hsl") || equals(name, "vsl");
    unsigned int names_at = bang || !slider ? 4 : 6;
    if ((bang && argc != 14) ||
        (slider && argc != 17 && argc != 18) ||
        (!bang && !slider && argc != 15))
        return false;
    if (!numbers(args, names_at, 10000) ||
        !numbers(args + names_at + 3, argc - names_at - 3, 1000000) ||
        !equals(args[names_at], "empty") ||
        !(equals(args[names_at + 1], "empty") || pod_name(args[names_at + 1])))
        return false;

    if (!integer(args[0], 4, 160, &spec.w))
        return false;
    spec.h = spec.w;
    spec.high = 1;
    if (bang)
        spec.kind = PDBOX_BANG;
    else if (slider)
    {
        if (!integer(args[1], 4, 120, &spec.h) ||
            !integer(args[2], -8191, 8191, &spec.low) ||
            !integer(args[3], -8191, 8191, &spec.high) ||
            spec.low >= spec.high ||
            !equals(args[4], "0"))
            return false;
        spec.kind = equals(name, "hsl") ? PDBOX_HSL : PDBOX_VSL;
    }
    else
    {
        int positions;
        if (!integer(args[3], 1, 16, &positions))
            return false;
        spec.kind = equals(name, "hradio") ? PDBOX_HRADIO : PDBOX_VRADIO;
        if (spec.kind == PDBOX_HRADIO)
            spec.w *= positions;
        else
            spec.h *= positions;
        spec.high = positions - 1;
    }
    return equals(args[names_at + 1], "empty") ||
           widget(patch, spec, args[names_at + 1]);
}

static bool object(char **words, unsigned int count, struct pdbox_graph *graph,
                   struct pdbox_patch *patch, struct pdbox_widget_spec spec,
                   struct pdbox_ports *ports)
{
    const char *name = words[0];
    char **args = words + 1;
    unsigned int argc = count - 1;
    static const struct {
        const char *name;
        unsigned char in, out, args;
    } builtins[] = {
        { "osc~", 2, 1, 1 }, { "phasor~", 2, 1, 1 }, { "noise~", 0, 1, 0 },
        { "sig~", 1, 1, 1 }, { "*~", 2, 1, 1 }, { "+~", 2, 1, 1 },
        { "-~", 2, 1, 1 }, { "lop~", 2, 1, 1 }, { "hip~", 2, 1, 1 },
        { "loadbang", 0, 1, 0 }, { "bang", 1, 1, 0 }, { "b", 1, 1, 0 },
        { "float", 2, 1, 1 }, { "f", 2, 1, 1 }, { "spigot", 2, 1, 1 },
    };

    for (unsigned int i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
    {
        if (equals(name, builtins[i].name))
        {
            *ports = (struct pdbox_ports){builtins[i].in, builtins[i].out};
            return argc <= builtins[i].args && numbers(args, argc, 8191);
        }
    }
    if (equals(name, "dac~"))
    {
        if (graph->dac || argc > 2)
            return false;
        graph->dac = true;
        *ports = (struct pdbox_ports){argc ? argc : 2, 0};
        for (unsigned int i = 0; i < argc; i++)
            if (!equals(args[i], "1") && !equals(args[i], "2"))
                return false;
        return true;
    }
    if (equals(name, "netreceive"))
    {
        if (graph->receiver || argc != 2 ||
            !equals(args[0], "3333") || !equals(args[1], "1"))
            return false;
        graph->receiver = true;
        *ports = (struct pdbox_ports){0, 1};
        return true;
    }
    if (equals(name, "route"))
    {
        if (argc == 0 || argc > 8)
            return false;
        for (unsigned int i = 0; i < argc; i++)
        {
            const char key = args[i][0];
            if (args[i][1] || !(key == 'b' || key == 'd' || key == 'w' ||
                key == 'f' || key == 'm' || key == 'r' || key == 'l' ||
                key == 'p'))
                return false;
        }
        *ports = (struct pdbox_ports){1, argc + 1};
        return true;
    }
    if (equals(name, "send") || equals(name, "s"))
    {
        *ports = (struct pdbox_ports){1, 0};
        if (argc != 1 || !pod_name(args[0]))
            return false;
        graph->sends[graph->objects - 1] = true;
        memcpy(graph->names[graph->objects - 1], args[0], strlen(args[0]) + 1);
        return true;
    }
    if (equals(name, "hsl") || equals(name, "vsl") || equals(name, "bng") ||
        equals(name, "hradio") || equals(name, "vradio"))
    {
        *ports = (struct pdbox_ports){1, 1};
        unsigned int old_count = patch->count;
        if (!iem_widget(name, args, argc, patch, spec))
            return false;
        if (patch->count != old_count)
            memcpy(graph->names[graph->objects - 1],
                   patch->widgets[old_count].name, PDBOX_NAME_BYTES);
        return true;
    }
    return false;
}

static bool record(char **words, unsigned int count, struct pdbox_graph *graph,
                   struct pdbox_patch *patch, bool first)
{
    if (first)
        return count == 7 && equals(words[0], "#N") &&
               equals(words[1], "canvas") && numbers(words + 2, 5, 4096);
    if (count < 2 || !equals(words[0], "#X"))
        return false;
    const char *kind = words[1];
    if (equals(kind, "connect"))
    {
        int from, outlet, to, inlet;
        graph->wiring = true;
        if (count != 6 || !graph->objects ||
            graph->connections == PDBOX_CONNECTIONS ||
            !integer(words[2], 0, graph->objects - 1, &from) ||
            !integer(words[3], 0, graph->ports[from].out - 1, &outlet) ||
            !integer(words[4], 0, graph->objects - 1, &to) ||
            !integer(words[5], 0, graph->ports[to].in - 1, &inlet))
            return false;
        graph->edges[graph->connections++] = (struct pdbox_edge){from, to};
        return true;
    }

    struct pdbox_widget_spec spec = {0};
    if (count < 5 || graph->objects == PDBOX_OBJECTS || graph->wiring ||
        !integer(words[2], -160, 160, &spec.x) ||
        !integer(words[3], -120, 120, &spec.y))
        return false;
    struct pdbox_ports *ports = &graph->ports[graph->objects++];
    if (equals(kind, "obj"))
        return object(words + 4, count - 4, graph, patch, spec, ports);
    if (equals(kind, "msg"))
    {
        *ports = (struct pdbox_ports){1, 1};
        return count == 5 && (equals(words[4], "bang") ||
                              numbers(words + 4, 1, 8191));
    }
    if (equals(kind, "text"))
    {
        char label[PDBOX_NAME_BYTES];
        size_t used = 0;
        for (unsigned int i = 4; i < count; i++)
        {
            size_t length = strlen(words[i]);
            if (used + (used != 0) + length >= sizeof(label))
                return false;
            if (used)
                label[used++] = ' ';
            memcpy(label + used, words[i], length);
            used += length;
        }
        label[used] = '\0';
        return widget(patch, spec, label);
    }
    if (equals(kind, "floatatom") && count == 11 &&
        integer(words[4], 1, 12, &spec.w) &&
        numbers(words + 5, 3, 8191) && equals(words[10], "-") &&
        (equals(words[9], "-") || pod_name(words[9])))
    {
        *ports = (struct pdbox_ports){equals(words[9], "-"), 1};
        spec.kind = PDBOX_NUMBER;
        spec.w *= 7;
        spec.h = 16;
        if (equals(words[9], "-"))
            return true;
        memcpy(graph->names[graph->objects - 1], words[9],
               strlen(words[9]) + 1);
        return widget(patch, spec, words[9]);
    }
    return false;
}

/* Named sends are edges too. Only pod_ widgets can receive them. */
static bool resolve_sends(struct pdbox_graph *graph)
{
    for (unsigned int from = 0; from < graph->objects; from++)
    {
        if (!graph->sends[from])
            continue;
        unsigned int to = 0;
        while (to < graph->objects && (graph->sends[to] ||
               !equals(graph->names[from], graph->names[to])))
            to++;
        if (to == graph->objects || graph->connections == PDBOX_CONNECTIONS)
            return false;
        graph->edges[graph->connections++] = (struct pdbox_edge){from, to};
    }
    return true;
}

/* Work backward from sinks. Bound even acyclic broadcast graphs, whose
 * individual messages can otherwise generate exponentially many dispatches. */
static bool graph_bounded(const struct pdbox_graph *graph)
{
    unsigned short work[PDBOX_OBJECTS] = {0};
    unsigned short pending[PDBOX_OBJECTS] = {0};
    unsigned char ready[PDBOX_OBJECTS];
    unsigned int head = 0, tail = 0;
    for (unsigned int i = 0; i < graph->connections; i++)
        pending[graph->edges[i].from]++;
    for (unsigned int i = 0; i < graph->objects; i++)
    {
        work[i] = 1;
        if (!pending[i])
            ready[tail++] = i;
    }
    while (head < tail)
    {
        unsigned int sink = ready[head++];
        for (unsigned int i = 0; i < graph->connections; i++)
        {
            struct pdbox_edge edge = graph->edges[i];
            if (edge.to != sink)
                continue;
            work[edge.from] += work[sink];
            if (work[edge.from] > PDBOX_DISPATCH_LIMIT)
                return false;
            if (!--pending[edge.from])
                ready[tail++] = edge.from;
        }
    }
    return tail == graph->objects;
}

static bool space(char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

bool pdbox_validate_patch(const char *data, size_t size,
                          struct pdbox_patch *patch)
{
    if (!size || size > PDBOX_PATCH_BYTES)
        return false;
    static struct pdbox_graph graph;
    memset(&graph, 0, sizeof(graph));
    char text[PDBOX_RECORD_BYTES];
    char *words[PDBOX_RECORD_ATOMS];
    size_t position = 0;
    bool first = true;
    patch->count = 0;

    while (position < size)
    {
        while (position < size && space(data[position]))
            position++;
        if (position == size)
            break;

        unsigned int used = 0, count = 0, length = 0;
        size_t start = position;
        while (position < size && data[position] != ';')
        {
            char c = data[position++];
            if (position - start >= sizeof(text))
                return false;
            if (space(c))
            {
                if (length)
                    text[used++] = '\0';
                length = 0;
                continue;
            }
            if (c < '!' || c > '~' || c == '\\' || c == '$' || c == ',')
                return false;
            if (!length)
            {
                if (count == PDBOX_RECORD_ATOMS)
                    return false;
                words[count++] = text + used;
            }
            if (++length >= PDBOX_NAME_BYTES)
                return false;
            text[used++] = c;
        }
        text[used] = '\0';
        if (position == size || !record(words, count, &graph, patch, first))
            return false;
        position++;
        first = false;
    }
    return !first && graph.objects && resolve_sends(&graph) &&
           graph_bounded(&graph);
}
