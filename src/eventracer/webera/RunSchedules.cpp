/*
   Copyright 2014 Software Reliability Lab, ETH Zurich

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <tr1/memory>
#include <tr1/shared_ptr.h>
#include <sys/stat.h>

#include "gflags/gflags.h"

#include "RaceApp.h"
#include "stringprintf.h"
#include "strutil.h"
#include "TraceReorder.h"

DEFINE_string(in_dir, "/tmp/",
        "Path to dir containing initial log files such as log.network.data");
DEFINE_string(in_schedule_file, "/tmp/schedule.data",
		"Filename with the schedules.");

DEFINE_string(site, "", "The website to replay");

DEFINE_string(replay_command, "/home/veselin/gitwk/WebERA/R5/clients/Replay/bin/replay %s %s -in_dir %s",
		"Command to run replay with twice %s for the site and the replay file.");
DEFINE_string(query_command, "/home/semadk/src/github/srl/WebERA/R4/utils/batch-report/report.py query %s %s",
        "Command to query/analyze the outcome of executing an event sequence.");

DEFINE_string(tmp_new_schedule_file, "/tmp/new_schedule.data",
        "Filename with the new schedule to be executed.");

DEFINE_string(tmp_er_log_file, "/tmp/out.ER_actionlog",
        "Filename with the new ER log.");
DEFINE_string(tmp_schedule_file, "/tmp/out.schedule.data",
		"Filename with the schedules.");
DEFINE_string(tmp_error_log, "/tmp/out.errors.log",
		"Filename with the schedules.");
DEFINE_string(tmp_png_file, "/tmp/out.screenshot.png",
		"Filename with the schedules.");
DEFINE_string(tmp_stdout, "/tmp/stdout.txt",
        "Standard output redirect of WebERA.");
DEFINE_string(tmp_network_log, "/tmp/log.network.data",
        "Filename with network data.");
DEFINE_string(tmp_time_log, "/tmp/log.time.data",
        "Filename with time data.");
DEFINE_string(tmp_random_log, "/tmp/log.random.data",
        "Filename with random data.");
DEFINE_string(tmp_status_log, "/tmp/status.data",
        "Filename with status data.");


DEFINE_string(out_dir, "/tmp/outdir",
        "Location of the output.");

DEFINE_bool(conflict_reversal_bound_oldstyle, false,
        "Use the original conflict-reversal bound calculation style");
DEFINE_int32(conflict_reversal_bound, 1,
        "Conflict-reversal bound.");
DEFINE_int32(iteration_bound, -1,
        "Maximum number of iterations. Use -1 for no limit.");

DEFINE_bool(fast_forward, true,
        "Apply fast-forward optimization.");
DEFINE_bool(same_state_reversal_opt, false,
        "Apply the same-state-reversal optimization.");

namespace {

bool MoveFile(const std::string& file, const std::string& out_dir) {
	if (system(StringPrintf("mv %s %s", file.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Cannot move %s\n", file.c_str());
        return false;
	}
	return true;
}

bool queryOutcome(const std::string& race_name) {

    if (!FLAGS_same_state_reversal_opt) {
        return false;
    }

    std::string query_command;
    StringAppendF(&query_command, FLAGS_query_command.c_str(), FLAGS_out_dir.c_str(), race_name.c_str());

    FILE* query = popen(query_command.c_str(), "r");

    if (!query) {
        fprintf(stderr, "Could not run command: %s\n", query_command.c_str());
        return false;
    }

    char buffer[1024];
    char* output = fgets(buffer, sizeof(buffer), query);

    pclose(query);

    if (strcmp(output, "LOW\n") == 0) {
        return true;
    }

    return false;
}

bool performSchedule(const std::string& race_name, const std::string& origin, const std::string& base_dir, const std::string& schedule,
                     std::string* executed_race_dir, std::string* executed_schedule_log, std::string* executed_er_log, bool* executed_reversal_is_benign) {

    /*
     * Executes replay command supplying the base dir with recorded data, URL, and path to the schedule.
     *
     * The function _moves_ all known output from the replay command to a race dir, and returns
     * - The path to the race dir
     * - The path to the executed schedule
     * - The path to the ER action log
     */

    setbuf(stdout, NULL);

    *executed_race_dir = "";
    *executed_schedule_log = "";
    *executed_er_log = "";
    *executed_reversal_is_benign = false;

    std::string out_dir = StringPrintf("%s/%s", FLAGS_out_dir.c_str(), race_name.c_str());

    struct stat st;
    if (FLAGS_fast_forward && stat(out_dir.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) == S_IFDIR) {

        *executed_race_dir = out_dir;
        *executed_schedule_log = StringPrintf("%s/%s", out_dir.c_str(), "schedule.data");
        *executed_er_log = StringPrintf("%s/%s", out_dir.c_str(), "ER_actionlog");

        if (stat(executed_schedule_log->c_str(), &st) == 0 && (st.st_mode & S_IFREG) == S_IFREG &&
                stat(executed_er_log->c_str(), &st) == 0 && (st.st_mode & S_IFREG) == S_IFREG) {

            *executed_reversal_is_benign = queryOutcome(race_name);

            fprintf(stdout, "Fast-forward execution.\n");
            return true;
        }
    }

    std::string error_out_dir = StringPrintf("%s/_%s", FLAGS_out_dir.c_str(), race_name.c_str());

    if (FLAGS_fast_forward && stat(error_out_dir.c_str(), &st) == 0 && (st.st_mode & S_IFDIR) == S_IFDIR) {

        fprintf(stdout, "Fast-forward (failed) execution.\n");
        return false;

    }

    std::string command;
	StringAppendF(&command, FLAGS_replay_command.c_str(),
            base_dir.c_str(), FLAGS_site.c_str(), schedule.c_str());
	command += " > ";
	command += FLAGS_tmp_stdout;

    fprintf(stdout, "Running %s\n", command.c_str());

    if (system(command.c_str()) != 0) {
		fprintf(stderr, "Could not run command: %s\n", command.c_str());

        // Move result files

        if (system(StringPrintf("mkdir -p %s", error_out_dir.c_str()).c_str()) != 0) {
            fprintf(stderr, "Could not create output dir %s. Set the flag --out_dir\n", error_out_dir.c_str());
            return false;
        }

        if (!MoveFile(schedule, error_out_dir)) return false;
        if (!MoveFile(FLAGS_tmp_stdout, error_out_dir + "/stdout")) return false;

        if (system(StringPrintf("echo \"%s\" > %s/origin", origin.c_str(), error_out_dir.c_str()).c_str()) != 0) {
            fprintf(stderr, "Could not create origin file\n");
            return false;
        }

        return false;
	}

    // Move result files

	if (system(StringPrintf("mkdir -p %s", out_dir.c_str()).c_str()) != 0) {
		fprintf(stderr, "Could not create output dir %s. Set the flag --out_dir\n", out_dir.c_str());
		return false;
	}
    if (!MoveFile(schedule, out_dir)) return false;
    if (!MoveFile(FLAGS_tmp_er_log_file, out_dir + "/ER_actionlog")) return false;
    if (!MoveFile(FLAGS_tmp_schedule_file, out_dir + "/schedule.data")) return false;
    if (!MoveFile(FLAGS_tmp_png_file, out_dir + "/screenshot.png")) return false;
    if (!MoveFile(FLAGS_tmp_error_log, out_dir + "/errors.log")) return false;
    if (!MoveFile(FLAGS_tmp_stdout, out_dir + "/stdout")) return false;
    if (!MoveFile(FLAGS_tmp_network_log, out_dir + "/log.network.data")) return false;
    if (!MoveFile(FLAGS_tmp_time_log, out_dir + "/log.time.data")) return false;
    if (!MoveFile(FLAGS_tmp_random_log, out_dir + "/log.random.data")) return false;
    if (!MoveFile(FLAGS_tmp_status_log, out_dir + "/status.data")) return false;

    if (system(StringPrintf("echo \"%s\" > %s/origin", origin.c_str(), out_dir.c_str()).c_str()) != 0) {
        fprintf(stderr, "Could not create origin file\n");
        return false;
    }

    // Output

    *executed_race_dir = out_dir;
    *executed_schedule_log = StringPrintf("%s/%s", out_dir.c_str(), "schedule.data");
    *executed_er_log = StringPrintf("%s/%s", out_dir.c_str(), "ER_actionlog");
    *executed_reversal_is_benign = queryOutcome(race_name);

	return true;
}

typedef int RaceID;
typedef int MixedEventId;
typedef size_t StrictEventID; // no placeholders or -1 error messages

typedef std::vector<StrictEventID> Schedule;
typedef std::vector<StrictEventID> ScheduleSuffix;
typedef std::vector<MixedEventId> ExecutableSchedule; // allow for -1 and -2 markers

typedef struct EATEntry_t {

    EATEntry_t(std::string base_race_output_dir,
               RaceID race_id,
               ScheduleSuffix schedule_suffix,
               ExecutableSchedule executable_schedule,
               std::tr1::shared_ptr<TraceReorder> reorder,
               const std::string& origin,
               int depth)
        : base_race_output_dir(base_race_output_dir)
        , race_id(race_id)
        , schedule_suffix(schedule_suffix)
        , executable_schedule(executable_schedule)
        , reorder(reorder)
        , origin(origin)
        , depth(depth)
    {
    }

    std::string base_race_output_dir;
    RaceID race_id;
    ScheduleSuffix schedule_suffix;
    ExecutableSchedule executable_schedule;
    std::tr1::shared_ptr<TraceReorder> reorder;
    std::string origin;
    int depth;

} EATEntry;

typedef std::vector<EATEntry> EAT;

typedef struct {

    std::string name;

    EAT eat;
    std::set<StrictEventID> visited;
    Schedule schedule;
    std::set<StrictEventID> sleepSet;
    bool m_race_first;
    bool m_race_second;
    int old_style_depth;

} State;

bool StateHasUnexploredEAT(State* state, const EATEntry** result) {

    for (size_t i = 0; i < state->eat.size(); ++i) {

        const EATEntry& entry = state->eat[i];

        if (state->visited.find(entry.schedule_suffix[0]) == state->visited.end()) {
            *result = &entry;
            return true;
        }
    }

    return false;
}

int EATMerge(std::vector<State*>* stack, size_t offset, const EATEntry& entry) {

    /*
     * Notice, the offset references the position in on the stack on which the schedule
     * should be rooted (or merged into)
     *
     * Thus, schedule[0+offset] references the first event, which should be rooted in stack[0+offset] and leads
     * to stack[1+offset].
     */

    size_t schedule_offset = 0;

    // next element on stack exist and next element matches next element in the schedule
    while (stack->size() > offset+1 &&
           entry.schedule_suffix.size() > schedule_offset &&
           stack->at(offset+1)->schedule.back() == entry.schedule_suffix[schedule_offset]) {

        ++offset;
        ++schedule_offset;
    }

    if (entry.schedule_suffix.size() <= schedule_offset) {
        // The schedule have already been explored
        return -1;
    }

    if (schedule_offset == 0) {
        stack->at(offset)->eat.push_back(entry);
    } else {
        EATEntry new_entry = entry;
        new_entry.schedule_suffix = ScheduleSuffix(entry.schedule_suffix.begin()+schedule_offset, entry.schedule_suffix.end());

        stack->at(offset)->eat.push_back(new_entry);
    }
    return schedule_offset;

}

void EATPropagate(std::vector<State*>* stack, size_t index) {

    State* state = stack->at(index);
    EAT old_eat;
    old_eat.swap(state->eat);

    while (!old_eat.empty()) {
        EATEntry eat = old_eat.back();
        old_eat.pop_back();

        EATMerge(stack, index, eat);
    }
}

void explore(const char* initial_schedule, const char* initial_base_dir) {

    if (FLAGS_conflict_reversal_bound_oldstyle) {
        fprintf(stdout, "Using old style conflict-reversal bounds\n");
    }

    int all_schedules = 0;
    int successful_reverses = 0;
    int successful_schedules = 0;
    int conflict_reversal_pruning = 0;
    int mini_sleep_set_pruning = 0;
    int conflict_reversal_dependency_pruning = 0;
    int same_state_reversal_opt = 0;

    std::vector<State*> stack;
    stack.reserve(5000);

    // Initialize

    std::tr1::shared_ptr<TraceReorder> init_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
    init_reorder->LoadSchedule(initial_schedule);

    ExecutableSchedule init_executable_schedule = init_reorder->GetSchedule();
    Schedule init_schedule = init_reorder->RemoveSpecialMarkers(init_executable_schedule);

    EATEntry init_eat(initial_base_dir, -1, init_schedule, init_executable_schedule, init_reorder, "", 0);

    State* initial_state = new State();
    initial_state->name = "";
    initial_state->m_race_first = false;
    initial_state->m_race_second = false;
    initial_state->eat.push_back(init_eat);
    initial_state->old_style_depth = 0;

    stack.push_back(initial_state);

    while (!stack.empty() && (FLAGS_iteration_bound == -1 || FLAGS_iteration_bound > successful_reverses)) {

        State* state = stack.back();

        /*
         * Check for unexplored schedule, follow it, and update EATs
         * Step 4 - (1 - 2)* - 3
         */

        const EATEntry* next_eat = NULL;
        if (StateHasUnexploredEAT(state, &next_eat) && next_eat != NULL) {

            // Step 4 -- Execute new schedule

            bool is_initial_execution = next_eat->race_id == -1;

            if (!is_initial_execution) {
                // don't count the initial execution
                ++successful_reverses;
            }

            std::string new_name = next_eat->race_id == -1 ? "base" : StringPrintf("%s_race%d", next_eat->origin.c_str(), next_eat->race_id);
            fprintf(stdout, "Reordering \"%s\" at depth %d (limit %d) offset %d: \n", new_name.c_str(), next_eat->depth, FLAGS_conflict_reversal_bound, (int)stack.size() - 1);

            next_eat->reorder->SaveSchedule(FLAGS_tmp_new_schedule_file.c_str(), next_eat->executable_schedule);

            std::string executed_base_dir;
            std::string executed_schedule_log;
            std::string executed_er_log;
            bool executed_reversal_is_benign;

            state->visited.insert(next_eat->schedule_suffix[0]);

            if (performSchedule(new_name, next_eat->origin, next_eat->base_race_output_dir, FLAGS_tmp_new_schedule_file.c_str(),
                                &executed_base_dir, &executed_schedule_log, &executed_er_log, &executed_reversal_is_benign)) {

                if (next_eat->race_id != -1) {
                    ++successful_schedules;
                }

                std::tr1::shared_ptr<TraceReorder> new_reorder = std::tr1::shared_ptr<TraceReorder>(new TraceReorder());
                new_reorder->LoadSchedule(executed_schedule_log.c_str());
                RaceApp new_race_app(0, executed_er_log, false);
                const VarsInfo& vinfo = new_race_app.vinfo();

                // Step 1 & Step 2 -- push states

                ExecutableSchedule executed_schedule = new_reorder->GetSchedule();
                Schedule schedule = new_reorder->RemoveSpecialMarkers(executed_schedule);

                int old_state_index = stack.size() - 1;
                int old_schedule_index = old_state_index - 1; // CAN BE -1 in the first iteration

                // The stack should be a prefix of executed_schedule
                // (with an empty zero element)
                // (and the prefix could have different event IDs).

                std::vector<int> eventToStackIndex(8000, -1);

                for (size_t i = 0; i < schedule.size(); ++i) {
                    if (eventToStackIndex.size()-1 < schedule[i]) {
                        eventToStackIndex.resize(eventToStackIndex.size()*2, -1);
                    }
                    eventToStackIndex[schedule[i]] = i+1;
                }

                int new_depth = is_initial_execution ? 0 : state->old_style_depth + 1;

                for (size_t i = old_schedule_index+1; i < schedule.size(); ++i) {
                    StrictEventID new_event_id = schedule[i];

                    State* new_state = new State();
                    new_state->name = new_name;

                    new_state->m_race_first = (i == next_eat->reorder->get_first_index());
                    new_state->m_race_second = (i == next_eat->reorder->get_second_index());

                    new_state->schedule = state->schedule;
                    new_state->schedule.push_back(new_event_id);
                    new_state->old_style_depth = new_depth;

                    state->visited.insert(new_event_id);

                    stack.push_back(new_state);
                    state = stack.back();
                }

                int current_depth = next_eat->depth;
                EATPropagate(&stack, old_state_index); // From this point on, next_eat is no longer valid

                // Step 3 -- update EATs

                fprintf(stdout, "Updating EATs... ");

                TraceReorder::Options options;
                options.include_change_marker = true;
                options.relax_replay_after_all_races = true;

                for (size_t race_id = 0; race_id < vinfo.races().size(); ++race_id) {

                    const VarsInfo::RaceInfo& race = vinfo.races()[race_id];

                    // Ignore covered races
                    if (!race.m_multiParentRaces.empty() || race.m_coveredBy != -1) continue;

                    // Conflict-reversal depth pruning
                    if (!FLAGS_conflict_reversal_bound_oldstyle &&
                            current_depth >= FLAGS_conflict_reversal_bound) {
                        ++conflict_reversal_pruning;
                        continue;
                    }

                    // Conflict-reversal depth pruning oldstyle
                    if (FLAGS_conflict_reversal_bound_oldstyle &&
                            eventToStackIndex[race.m_event1] > 0 &&
                            stack[eventToStackIndex[race.m_event1]-1]->old_style_depth >= FLAGS_conflict_reversal_bound) {
                        ++conflict_reversal_pruning;
                        printf("Reversing %d<->%d where %d is at index %d with depth %d, old index is %d\n", race.m_event1, race.m_event2, race.m_event1, eventToStackIndex[race.m_event1], stack[eventToStackIndex[race.m_event1]-1]->old_style_depth, old_state_index);
                        continue;
                    }

                    // Do not re-insert races with events we have already explored.
                    MixedEventId event_prior_to_reversal = old_schedule_index != -1 ? (int)schedule[old_schedule_index] : -1;
                    if (event_prior_to_reversal > race.m_event2) {  // event ids are sequential
                        continue;
                    }

                    // Do not reverse the race we have just explored
                    // Small optimization, a form of limit sleep set
                    if (eventToStackIndex[race.m_event2] - eventToStackIndex[race.m_event1] == 1 &&
                            stack.at(eventToStackIndex[race.m_event1])->m_race_first &&
                            stack.at(eventToStackIndex[race.m_event2])->m_race_second) {
                        ++mini_sleep_set_pruning;
                        continue;
                    }

                    if (FLAGS_same_state_reversal_opt && executed_reversal_is_benign &&
                            !stack.at(eventToStackIndex[race.m_event1])->m_race_first &&
                            !stack.at(eventToStackIndex[race.m_event1])->m_race_second &&
                            !stack.at(eventToStackIndex[race.m_event2])->m_race_first &&
                            !stack.at(eventToStackIndex[race.m_event2])->m_race_second) {
                        ++same_state_reversal_opt;
                        continue;
                    }

                    ++all_schedules;

                    ExecutableSchedule pending_executable_schedule;
                    new_reorder->GetScheduleFromRace(vinfo, race_id, new_race_app.graph(), options, &pending_executable_schedule);
                    Schedule pending_schedule = new_reorder->RemoveSpecialMarkers(pending_executable_schedule);

                    ScheduleSuffix pending_schedule_suffix(pending_schedule.begin() + eventToStackIndex[race.m_event1]-1, pending_schedule.end());

                    EATEntry pending_entry(executed_base_dir, race_id, pending_schedule_suffix, pending_executable_schedule, new_reorder, new_name, current_depth+1);
                    EATMerge(&stack, eventToStackIndex[race.m_event1]-1, pending_entry);

                }

                fprintf(stdout, "DONE\n");

            } // End successful execution

        } else { // End if has unexplored EAT

            // backtrack

            delete state;

            state = stack.back();
            stack.pop_back();

        }

    }

    if (FLAGS_iteration_bound != -1 && FLAGS_iteration_bound <= successful_reverses) {
        printf("WARNING: Stopped iteration: Iteration limit reached.\n");
    }

    printf("Statistics: conflict-reversal-pruning: %d\n", conflict_reversal_pruning);
    printf("Statistics: mini-sleep-set-pruning: %d\n", mini_sleep_set_pruning);
    printf("Statistics: conflict-reversal-dependency-pruning: %d\n", conflict_reversal_dependency_pruning);
    printf("Statistics: same-state-reversal-pruning: %d\n", same_state_reversal_opt);
    printf("Tried %d schedules. %d generated, %d successful\n",
            all_schedules, successful_reverses, successful_schedules);

}

}  // namespace


int main(int argc, char* argv[]) {
	google::ParseCommandLineFlags(&argc, &argv, true);

	if (FLAGS_site.empty()) {
		fprintf(stderr, "  --site is a mandatory parameter.\n");
		return -1;
	}

    explore(FLAGS_in_schedule_file.c_str(), FLAGS_in_dir.c_str());

	return 0;
}
