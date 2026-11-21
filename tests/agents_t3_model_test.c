/* Host tests for the T3 additions to the HerdRemote model. GPL version 2+. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "herdremote_model.h"

static struct hr_model model, before;

static int apply(const char *frame)
{
    return hr_model_apply(&model, frame, strlen(frame));
}

static void reject(const char *frame)
{
    memcpy(&before, &model, sizeof before);
    assert(apply(frame) == 0);
    assert(memcmp(&before, &model, sizeof before) == 0);
}

static void test_workspaces(void)
{
    hr_model_reset(&model);
    assert(apply("W\t1\nw1\tf\t1\t0\t0\t0\therdr") == 'W');
    assert(model.nws == 1 && model.ws[0].tab1 == 'w');
    reject("W\t1\n#\tT3");
    reject("W\t1\n#\tT3\n#\tT3\nt3p1\t-\t0\t1\t0\t0\tT3");
    reject("W\t1\n#\tT3\nw1\tf\t1\t0\t0\t0\therdr");
    reject("W\t2\n#\tT3\nt3p1\t-\t0\t1\t0\t0\tProject");
    assert(apply("W\t2\nw1\tf\t1\t0\t0\t0\therdr\n#\tT3\n"
                 "t3p1\t-\t0\t1\t0\t0\tProject") == 'W');
    assert(model.nws == 2 && model.ws_shown_changed);
    assert(!strcmp(model.ws[1].id, "t3p1") && model.ws[1].tab1 == 'b');
    assert(apply("S\t1\nt3p1\tw\tb") == 'S');
    assert(model.ws[1].tab1 == 'w' && model.ws[1].other == 'b');

    char frame[2048] = "W\t32";
    size_t used = strlen(frame);
    for (int i = 0; i < HR_MAX_WS; i++)
        used += snprintf(frame + used, sizeof frame - used,
                         "%s\n%s%d\t-\t0\t0\t0\t0\tProject", i == 24 ?
                         "\n#\tT3" : "", i < 24 ? "w" : "t3p", i);
    assert(apply(frame) == 'W' && model.nws == HR_MAX_WS);
    assert(!strcmp(model.ws[24].id, "t3p24"));
    assert(!strcmp(model.ws[31].id, "t3p31"));
    assert(apply("W\t1\nw1\t-\t0\t0\t1\t0\therdr") == 'W');
    assert(model.nws == 1 && model.ws_shown_changed);
}

static void test_actions(void)
{
    static const char valid[] =
        "K\tt3t1\t2\ninterrupt\tInterrupt turn\t1\nreply1\t继续\t0";
    hr_model_reset(&model);
    reject(valid);
    strcpy(model.actions_target, "t3t1");
    reject("K\tt3t2\t1\ninterrupt\tInterrupt turn\t1");
    reject("K\tt3t1\t13");
    reject("K\tt3t1\t-");
    reject("K\tt3t1\t2\ninterrupt\tInterrupt turn\t1");
    reject("K\tt3t1\t1\ninterrupt\tInterrupt turn\t01");
    reject("K\tt3t1\t1\ninterrupt\tInterrupt turn\t1\textra");
    reject("K\tt3t1\t1\ninterrupt\tInterrupt turn\r\t1");
    reject("K\tt3t1\t1\n12345678901234567\tLabel\t0");
    reject("K\tt3t1\t1\nreply1\t12345678901234567890123456789012345678901\t0");
    reject("K\tt3t1\t1\ninterrupt\tInterrupt turn\t1\nextra");
    reject("K\tt3t1\t2\nreply1\tYes\t0\nreply1\tNo\t0");
    assert(apply(valid) == 'K');
    assert(model.actions_valid && model.nactions == 2);
    assert(!strcmp(model.actions[0].id, "interrupt"));
    assert(model.actions[0].confirm && !model.actions[1].confirm);
    assert(!strcmp(model.actions[1].label, "继续"));
    reject("K\tt3t1\t0");

    model.actions_valid = false;
    strcpy(model.actions_target, "w1:p2");
    char frame[512] = "K\tw1:p2\t12";
    size_t used = strlen(frame);
    for (int i = 0; i < HR_MAX_ACTIONS; i++)
        used += snprintf(frame + used, sizeof frame - used,
                         "\nact%d\tAction %d\t0", i, i);
    assert(apply(frame) == 'K' && model.nactions == HR_MAX_ACTIONS);
    assert(!strcmp(model.actions[11].id, "act11"));
    model.actions_valid = false;
    assert(apply("K\tw1:p2\t0") == 'K' && model.nactions == 0);
}

int main(void)
{
    test_workspaces();
    test_actions();
    puts("agents_t3_model_test: all passed");
    return 0;
}
