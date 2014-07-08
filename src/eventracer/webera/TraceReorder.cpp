/*
   Copyright 2013 Software Reliability Lab, ETH Zurich

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


#include "TraceReorder.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <map>

#include "file.h"

#include "VarsInfo.h"

TraceReorder::TraceReorder() {
}

TraceReorder::~TraceReorder() {
}

void TraceReorder::LoadSchedule(const char* filename) {
	fprintf(stderr, "Loading schedule %s...", filename);
	m_actions.clear();
	FILE* f = fopen(filename, "rt");
	std::string line;
	while (ReadLine(f, &line)) {
		int node_id;
		if (sscanf(line.c_str(), "%d;", &node_id) == 1) {
			while (node_id >= static_cast<int>(m_actions.size())) {
				m_actions.push_back(std::string(""));
			}

			m_actions[node_id] = std::string(strchr(line.c_str(), ';') + 1);
            m_schedule.push_back(node_id);
		}
	}
	fclose(f);
	fprintf(stderr, "Done\n");
}

void TraceReorder::SaveSchedule(const char* filename, const std::vector<int>& schedule) const {
	fprintf(stderr, "Saving schedule to %s...\n", filename);
	FILE* f = fopen(filename, "wt");
	int maxi = m_actions.size();
	for (size_t i = 0; i < schedule.size(); ++i) {
		if (schedule[i] == -2) {
			fprintf(f, "<change>\n");
		}
		if (schedule[i] == -1) {
			fprintf(f, "<relax>\n");
		}
		if (schedule[i] >= 0 && schedule[i] < maxi) {
			const std::string& s = m_actions[schedule[i]];
			if (!s.empty()) {
				fprintf(f, "%d;%s\n", schedule[i], s.c_str());
			}
		}
	}
	fclose(f);
}

bool TraceReorder::GetScheduleFromRace(
		const VarsInfo& vinfo,
        const int race_id,
		const SimpleDirectedGraph& graph,
		const Options& options,
        std::vector<int>* new_schedule) const {

    if (race_id < 0 || (size_t)race_id >= vinfo.races().size()) {
        return false;
    }

    new_schedule->clear();

    const VarsInfo::RaceInfo& race = vinfo.races()[race_id];
    const EventGraphInterface* hb = vinfo.fast_event_graph();

    /*
     * Takes a schedule on the form axbyc, where a b and c are event sequences and
     * x and y are racing events (identified by the ``race_id``).
     *
     * This algorithm reorders ``schedule``, reversing x and y. The resulting
     * schedule is on the form ab'yxb''c, where b' is the sequence of events in b
     * which are independent with x, and b'' is the sequence of events dependent
     * on x.
     */

    // Emit ``a`` until we see x

    size_t schedule_pos = 0;

    for (; schedule_pos < m_schedule.size() && m_schedule[schedule_pos] != race.m_event1; ++schedule_pos) {
        new_schedule->push_back(m_schedule[schedule_pos]);
    }

    // Skip x

    std::vector<int> bprimeprime;

    bprimeprime.push_back(m_schedule[schedule_pos]);
    ++schedule_pos;

    // Emit b' until we see y

    for (; schedule_pos < m_schedule.size() && m_schedule[schedule_pos] != race.m_event2; ++schedule_pos) {

        // Check if the current event, u, depends on x.
        // Dependency is transitive, so u depends on x if
        // - x happens before u
        // - x races with u
        // - an event in b'' depends on u

        bool depends = false;

        for (size_t i = 0; i < bprimeprime.size(); ++i) {

            // happens before
            if (hb->areOrdered(bprimeprime[i], m_schedule[schedule_pos])) {
                depends = true;
                break;
            }

            // races
            const VarsInfo::AllRaces& races = vinfo.races();
            VarsInfo::AllRaces::const_iterator it = races.begin();
            for (; it != races.end(); ++it) {
                if (it->m_event1 == bprimeprime[i] && it->m_event2 == m_schedule[schedule_pos]) {
                    depends = true;
                    break;
                }
            }

        }

        if (depends) {
            bprimeprime.push_back(m_schedule[schedule_pos]);
        } else {
            new_schedule->push_back(m_schedule[schedule_pos]);
        }
    }

    // <change> marker

    if (options.include_change_marker) {
        new_schedule->push_back(-2);
    }

    // Emit y

    new_schedule->push_back(m_schedule[schedule_pos]);
    ++schedule_pos;

    // Emit <relax>

    if (options.relax_replay_after_all_races) {
        new_schedule->push_back(-1);
    }

    // Emit x and b''

    for (size_t i = 0; i < bprimeprime.size(); ++i) {
        new_schedule->push_back(bprimeprime[i]);
    }

    // Emit c

    for (; schedule_pos < m_schedule.size(); ++schedule_pos) {
        new_schedule->push_back(m_schedule[schedule_pos]);
    }

    // Done

    return true;
}

void TraceReorder::GetScheduleWithoutRace(
        std::vector<int>* new_schedule) const {

    for (size_t i = 0; i < m_schedule.size(); ++i) {
        new_schedule->push_back(m_schedule[i]);
    }

}

