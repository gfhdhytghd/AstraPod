"""Exercise question transitions with production model/parser and mock UI/transport."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = Path(sys.argv[1]).resolve()
OUTPUT.mkdir(parents=True, exist_ok=True)
SOURCE = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else ROOT
SYSTEM = SOURCE / 'apps-ipod/screens/system'
SCREEN = (SYSTEM / 'agents_screen.c').read_text()


def function(name):
    match = re.search(r'(?m)^static [\w *]+\b' + name + r'\([^;{}]*\)\s*\{', SCREEN)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (SCREEN[end] == '{') - (SCREEN[end] == '}')
        end += 1
    return SCREEN[match.start():end]


prelude = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "herdremote_model.h"
#include "podlink.h"
#define HZ 100
#define TIME_BEFORE(a,b) ((long)((unsigned long)(a)-(unsigned long)(b))<0)
#define TIME_AFTER(a,b) TIME_BEFORE(b,a)
VIEWS
struct gui_synclist {int sel;};
struct row {int kind;};
static struct hr_model model;
static struct hr_popped popped;
static struct row rows[4], question_from_row;
static int nrows, alert_choice, question_choice, page_scene, input_top;
static enum view view, question_from;
static long current_tick, alert_at, last_message, usage_at, pending_until, question_next_until;
static char question_id[12], question_pane[16], question_saved_detail[16], detail_pane[16];
static char question_agent[44], question_tab[44], question_line[124];
static char focus_want[16], talk_req[8], focus_req[8], action_req[8], pending_req[8], key_req[8];
static struct {int nruns;} key_runs;
static bool select_down, modal_open, alert_direction_ready, talk_held, talk_discarded, talk_failed, focus_for_detail;
static bool cache_valid, detail_ready, pending_answer, question_custom, question_closing;
static bool question_resolving;
static int commands, focus_commands, wakes, notifications, errors;
static char last_command[8], last_args[32];
static const struct hr_question *current_question(void);
static void enter_answer(struct gui_synclist *list);
static void ui_scene_cancel(int *scene) {*scene=0;}
static bool mutating_ready(void) {return true;}
static void backlight_on(void) {wakes++;}
static void agents_background_update(struct hr_model *m,int type,bool foreground)
{assert(m==&model && type && !foreground); notifications++;}
static void agents_background_disconnected(void) {}
static void talk_result(const char *text,bool failed) {(void)text; errors+=failed;}
static bool take_talk_result(void) {return false;}
static int gui_synclist_get_sel_pos(struct gui_synclist *list) {return list->sel;}
static void show(struct gui_synclist *list,int row) {list->sel=row;}
static void show_row(struct gui_synclist *list,const struct row *row) {show(list,row->kind);}
static void show_same(struct gui_synclist *list) {(void)list;}
static void draw_question_overlay(struct gui_synclist *list,int motion) {(void)list;(void)motion;}
static void forget_keys(void) {key_req[0]=0;key_runs.nruns=0;}
static void finish_talk(bool discard) {assert(discard);talk_held=false;}
static void subscribe_detail(const char *p) {assert(p==detail_pane);}
static bool queue_focus(const char *name,const char *pane,bool detail)
{assert(!strcmp(name,"fp") && pane[0] && detail);strcpy(focus_req,"1");focus_commands++;return true;}
static bool queue_request(const char *name,const char *args)
{
    strcpy(last_command,name);strcpy(last_args,args);strcpy(pending_req,"2");
    pending_until=current_tick+8*HZ;commands++;return true;
}
'''.replace('VIEWS', re.search(r'enum view \{[^}]+\};', SCREEN).group())

checks = r'''
static void reset(enum view screen)
{
    hr_model_reset(&model);
    memset(&popped,0,sizeof popped);
    view=screen;current_tick=100;
    question_id[0]=question_pane[0]=detail_pane[0]=focus_req[0]=pending_req[0]=talk_req[0]=0;
    talk_held=pending_answer=question_custom=question_closing=question_resolving=false;
    detail_ready=modal_open=select_down=false;
    commands=focus_commands=wakes=notifications=errors=0;
    cache_valid=true;
}
static void receive(const char *message)
{assert(hr_model_apply(&model,message,strlen(message))==message[0]);}
static void result(struct gui_synclist *list)
{receive("R\t2\tok\tAccepted");take_result(list);}
int main(void)
{
    FILE *file=fopen(REAL_PAYLOAD,"rb");assert(file);
    char payload[2048];size_t size=fread(payload,1,sizeof payload,file);
    assert(!ferror(file)&&feof(file));fclose(file);
    struct gui_synclist list={0};
    for (enum view screen=VIEW_SPACES;screen<=VIEW_DETAIL;screen++) {
        reset(screen);
        if (screen==VIEW_DETAIL) strcpy(detail_pane,"w13:p1");
        struct podlink_message message={.type=PODLINK_TEXT,.data=(unsigned char *)payload,.length=size};
        background_link(&message);
        assert(notifications==1&&wakes==0&&view==screen&&model.nquestions==1);
        assert(!strcmp(model.questions[0].id,"e0298fe2"));
        assert(model.questions[0].nlines==2&&model.questions[0].options[2].custom);
        talk_held=true;pop_question(&list);assert(view==screen);
        talk_held=false;strcpy(talk_req,"1");pop_question(&list);assert(view==screen);
        talk_req[0]=0;select_down=true;pop_question(&list);
        assert(view==VIEW_ALERT&&wakes==1&&!select_down&&alert_choice==2);
        alert_activate(&list,2);assert(view==VIEW_ALERT&&commands==0);
        leave_question(&list);pop_question(&list);
        assert(view==screen&&model.nquestions==1&&commands==0);
        open_question(&list,&model.questions[0]);current_tick+=HZ/4;
        alert_activate(&list,2);assert(view==VIEW_QUESTION&&focus_commands==1);
        answer_number(3);assert(commands==0);
        focus_req[0]=0;detail_ready=true;answer_number(3);
        assert(commands==1&&question_custom&&!strcmp(last_args,"e0298fe2\t3"));
        result(&list);assert(view==VIEW_INPUT&&!question_custom);
        strcpy(key_req,"edit");close_input(true);assert(commands==1);
        key_req[0]=0;close_input(false);
        assert(commands==2&&!strcmp(last_command,"qi")&&!strcmp(last_args,"e0298fe2\tesc"));
        result(&list);assert(view==screen&&model.nquestions==1);
    }
    reset(VIEW_DETAIL);
    receive("Q\tt3a1\tt3t1\tt3p1\t-\tTest\n?\tRun tests?\no\t1\tApprove\no\t2\tDecline");
    pop_question(&list);assert(view==VIEW_ALERT);
    current_tick+=HZ/4-1;alert_activate(&list,0);assert(commands==0);
    current_tick++;alert_activate(&list,1);
    assert(commands==1&&focus_commands==0&&!strcmp(last_args,"t3a1\t2"));
    result(&list);receive("X\tt3a1");advance_question(&list);assert(view==VIEW_DETAIL);
    reset(VIEW_DETAIL);
    receive("Q\tt3q1\tt3t1\tt3p1\t-\tTest\n?\tPick one\no\t1\tRun\no\t2\tWait");
    pop_question(&list);assert(view==VIEW_QUESTION&&focus_commands==0);
    answer_number(1);result(&list);receive("X\tt3q1");
    advance_question(&list);assert(view==VIEW_QUESTION);
    receive("Q\tt3q2\tt3t1\tt3p1\t-\tTest\n?\tPick again\no\t1\tRun\no\t2\tWait");
    advance_question(&list);assert(view==VIEW_QUESTION&&!strcmp(question_id,"t3q2"));
    leave_question(&list);assert(commands==1&&view==VIEW_DETAIL&&model.nquestions==1);
    open_question(&list,&model.questions[0]);dismiss_question();
    assert(commands==2&&!strcmp(last_args,"t3q2\t-2"));
    puts("Agents questions: exact UTF-8 fixture, deferral, grace, focus, legacy custom input, T3 fast choice, sequence and explicit dismiss PASS");
}
'''

functions = ('is_t3', 'take_action_result', 'background_link', 'current_question',
             'quick_choice', 'open_question', 'leave_question', 'dismiss_question',
             'pop_question', 'enter_answer', 'answer_number', 'close_input',
             'alert_activate', 'take_result', 'advance_question')
probe = OUTPUT / 'questions.c'
probe.write_text(prelude + '\n'.join(map(function, functions)) + checks)
payload = ROOT / 'tools/tests/fixtures/fu21-hang-q.bin'
subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-ftrivial-auto-var-init=pattern', f'-DREAL_PAYLOAD="{payload}"',
                '-I' + str(SYSTEM), '-I' + str(SOURCE / 'firmware/export'), str(probe),
                str(SYSTEM / 'herdremote_model.c'), '-o', str(OUTPUT / 'questions')], check=True)
subprocess.run([str(OUTPUT / 'questions')], check=True)
