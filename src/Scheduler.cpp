/*
 *   Scheduler Class for Falcon Player (FPP)
 *
 *   Copyright (C) 2013-2018 the Falcon Player Developers
 *      Initial development by:
 *      - David Pitts (dpitts)
 *      - Tony Mace (MyKroFt)
 *      - Mathew Mrosko (Materdaddy)
 *      - Chris Pinkham (CaptainMurdoch)
 *      For additional credits and developers, see credits.php.
 *
 *   The Falcon Player (FPP) is free software; you can redistribute it
 *   and/or modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "fpp-pch.h"

#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <string.h>

#include "command.h"
#include "fpp.h"
#include "playlist/Playlist.h"
#include "Scheduler.h"
#include "SunSet.h"

#define SCHEDULE_FILE     "/home/fpp/media/config/schedule.json"
#define SCHEDULE_FILE_CSV "/home/fpp/media/schedule"

Scheduler *scheduler = NULL;


static int GetWeeklySeconds(int day, int hour, int minute, int second)
{
  int weeklySeconds = (day*SECONDS_PER_DAY) + (hour*SECONDS_PER_HOUR) + (minute*SECONDS_PER_MINUTE) + second;

  if (weeklySeconds >= SECONDS_PER_WEEK)
    weeklySeconds -= SECONDS_PER_WEEK;

  return weeklySeconds;
}


void SchedulePlaylistDetails::SetTimes(time_t currTime, int nowWeeklySeconds)
{
    actualStartTime = currTime;
    actualStartWeeklySeconds = nowWeeklySeconds;

    scheduledStartTime = currTime - (nowWeeklySeconds - startWeeklySeconds);

    if (nowWeeklySeconds < startWeeklySeconds)
        scheduledStartTime -= 7 * 24 * 60 * 60;

    scheduledEndTime = currTime + (endWeeklySeconds - nowWeeklySeconds);

    if (endWeeklySeconds < startWeeklySeconds)
        scheduledEndTime += 7 * 24 * 60 * 60;

    actualEndTime = scheduledEndTime;
}

/////////////////////////////////////////////////////////////////////////////

Scheduler::Scheduler()
  : m_loadSchedule(true),
	m_CurrentScheduleHasbeenLoaded(0),
	m_NextScheduleHasbeenLoaded(0),
	m_nowWeeklySeconds2(0),
	m_lastLoadDate(0),
	m_lastProcTime(0),
	m_lastLoadTime(0),
	m_lastCalculateTime(0),
    m_forcedNextPlaylist(SCHEDULE_INDEX_INVALID)
{
	ConvertScheduleFile();

	RegisterCommands();

	m_lastProcTime = time(NULL);
	LoadScheduleFromFile();
}

Scheduler::~Scheduler()
{
}

void Scheduler::ScheduleProc(void)
{
  time_t procTime = time(NULL);

  if ((m_lastLoadDate != GetCurrentDateInt()) ||
      ((procTime - m_lastProcTime) > 5))
  {
    m_loadSchedule = true;
    if (playlist->getPlaylistStatus() == FPP_STATUS_IDLE)
      m_CurrentScheduleHasbeenLoaded = 0;

    m_NextScheduleHasbeenLoaded = 0;
  }

  m_lastProcTime = procTime;

  if (m_loadSchedule)
	LoadScheduleFromFile();

  if ((!m_CurrentScheduleHasbeenLoaded) || (!m_NextScheduleHasbeenLoaded))
	SchedulePrint();

  if(!m_CurrentScheduleHasbeenLoaded)
    LoadCurrentScheduleInfo();

  if(!m_NextScheduleHasbeenLoaded)
    LoadNextScheduleInfo();

  switch(playlist->getPlaylistStatus())
  {
    case FPP_STATUS_IDLE:
      if (m_currentSchedulePlaylist.ScheduleEntryIndex != SCHEDULE_INDEX_INVALID)
        PlayListLoadCheck();
      break;
    case FPP_STATUS_PLAYLIST_PLAYING:
      if (playlist->WasScheduled())
        PlayListStopCheck();
      else if (m_currentSchedulePlaylist.ScheduleEntryIndex != SCHEDULE_INDEX_INVALID)
        PlayListLoadCheck();
      break;
    default:
      break;

  }
}

void Scheduler::CheckIfShouldBePlayingNow(int ignoreRepeat)
{
    if (m_loadSchedule) {
        LoadScheduleFromFile();
    }
    
    int i,dayCount;
    time_t currTime = time(NULL);
    struct tm now;

    localtime_r(&currTime, &now);

    int nowWeeklySeconds = GetWeeklySeconds(now.tm_wday, now.tm_hour, now.tm_min, now.tm_sec);
    for( i = 0; i < m_Schedule.size(); i++) {
        bool ir = ignoreRepeat;
        if (m_forcedNextPlaylist == i) {
            ir = true;
        }
        
		// only check schedule entries that are enabled and set to repeat.
		// Do not start non repeatable entries
		if ((m_Schedule[i].enabled) &&
			(m_Schedule[i].repeat || ir) &&
			(CurrentDateInRange(m_Schedule[i].startDate, m_Schedule[i].endDate)))
		{
            int j = 0;
            for (auto & startEnd : m_Schedule[i].startEndSeconds) {
				// If end is less than beginning it means this entry wraps from Saturday to Sunday,
				// otherwise, end should always be higher than start even if end is the next morning.
				if (((startEnd.second < startEnd.first) &&
					 ((nowWeeklySeconds >= startEnd.first) ||
					  (nowWeeklySeconds < startEnd.second))) ||
					((nowWeeklySeconds >= startEnd.first) && (nowWeeklySeconds < startEnd.second)))
				{
					LogWarn(VB_SCHEDULE, "Should be playing now - schedule index = %d weekly index= %d\n", i, j);
					m_currentSchedulePlaylist.ScheduleEntryIndex = i;
                    m_currentSchedulePlaylist.startWeeklySeconds = startEnd.first;
                    m_currentSchedulePlaylist.endWeeklySeconds = startEnd.second;

					m_CurrentScheduleHasbeenLoaded = 1;

					m_currentSchedulePlaylist.entry = m_Schedule[i];
					m_currentSchedulePlaylist.SetTimes(currTime, nowWeeklySeconds);

                    m_forcedNextPlaylist = SCHEDULE_INDEX_INVALID;
					playlist->Play(m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].playlist.c_str(),
						0, m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].repeat, 1);

					if (m_loadSchedule)
						LoadScheduleFromFile();

					SchedulePrint();
					LoadNextScheduleInfo();

					return;
				}
                j++;
			}
		}
    }

    if (m_loadSchedule)
        LoadScheduleFromFile();

    SchedulePrint();
    LoadCurrentScheduleInfo();
    LoadNextScheduleInfo();
}

std::string Scheduler::GetPlaylistThatShouldBePlaying(int &repeat)
{
	int i,j,dayCount;
	time_t currTime = time(NULL);
	struct tm now;

	repeat = 0;

	localtime_r(&currTime, &now);

    if (playlist->getPlaylistStatus() != FPP_STATUS_IDLE) {
        if (m_currentSchedulePlaylist.ScheduleEntryIndex != SCHEDULE_INDEX_INVALID) {
            repeat = m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].repeat;
            return m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].playlist;
        } else {
            return "";
        }
    }

	int nowWeeklySeconds = GetWeeklySeconds(now.tm_wday, now.tm_hour, now.tm_min, now.tm_sec);
	for( i = 0; i < m_Schedule.size(); i++)
	{
		if ((m_Schedule[i].enabled) &&
			(CurrentDateInRange(m_Schedule[i].startDate, m_Schedule[i].endDate))) {
            for (auto & startEnd : m_Schedule[i].startEndSeconds) {
				// If end is less than beginning it means this entry wraps from Saturday to Sunday,
				// otherwise, end should always be higher than start even if end is the next morning.
                if (((startEnd.second < startEnd.first) &&
					 ((nowWeeklySeconds >= startEnd.first) ||
                      (nowWeeklySeconds < startEnd.second))) ||
					((nowWeeklySeconds >= startEnd.first) && (nowWeeklySeconds < startEnd.second)))
				{
					repeat = m_Schedule[i].repeat;
					return m_Schedule[i].playlist;
				}
			}
		}
	}

	return "";
}

int Scheduler::GetNextScheduleEntry(int *weeklySecondIndex, bool future)
{
    int i,dayCount;
    int leastWeeklySecondDifferenceFromNow=SECONDS_PER_WEEK;
    int difference;
    int nextEntryIndex = SCHEDULE_INDEX_INVALID;
    int minDiff = future ? 1 : 0;
    time_t currTime = time(NULL);
    struct tm now;

    localtime_r(&currTime, &now);
    
    int curPlayIdx = m_currentSchedulePlaylist.ScheduleEntryIndex;
    if (curPlayIdx == SCHEDULE_INDEX_INVALID) {
        curPlayIdx = 0xFFFF;
    }

    std::string playlistFile;
    int nowWeeklySeconds = GetWeeklySeconds(now.tm_wday, now.tm_hour, now.tm_min, now.tm_sec);
    for (i = 0; i < m_Schedule.size(); i++) {
        if (!m_Schedule[i].enabled)
            continue;

        playlistFile = getPlaylistDirectory();
        playlistFile += "/";
        playlistFile += m_Schedule[i].playlist + ".json";

        std::string warning = "Scheduled Playlist '";
        warning += m_Schedule[i].playlist + "' does not exist";

        if (FileExists(playlistFile)) {
            WarningHolder::RemoveWarning(warning);
        } else {
            WarningHolder::AddWarning(warning);
            m_Schedule[i].enabled = false;
            continue;
        }

        if (CurrentDateInRange(m_Schedule[i].startDate, m_Schedule[i].endDate)) {
            int j = 0;
            for (auto & startEnd : m_Schedule[i].startEndSeconds) {
                int start = startEnd.first;
                int end = startEnd.second;
                if (i > curPlayIdx) {
                    //lower priority, adjust start time until after current playlist is done
                    if (start < m_currentSchedulePlaylist.endWeeklySeconds) {
                        if (end > m_currentSchedulePlaylist.endWeeklySeconds) {
                            start = m_currentSchedulePlaylist.endWeeklySeconds;
                        }
                    }
                }
                difference = GetWeeklySecondDifference(nowWeeklySeconds, start);
                if ((difference >= minDiff) && (difference < leastWeeklySecondDifferenceFromNow)) {
                    leastWeeklySecondDifferenceFromNow = difference;
                    nextEntryIndex = i;
                    *weeklySecondIndex = j;
                }
                j++;
            }
        }
    }
    LogDebug(VB_SCHEDULE, "nextEntryIndex = %d, least diff = %d, weekly index = %d, (%s)\n",nextEntryIndex,leastWeeklySecondDifferenceFromNow,*weeklySecondIndex, future ? "future" : "current");
    return nextEntryIndex;
}

void Scheduler::ReloadScheduleFile(void)
{
	m_loadSchedule = true;
}

void Scheduler::ReLoadCurrentScheduleInfo(void)
{
    m_CurrentScheduleHasbeenLoaded = 0;
}

void Scheduler::ReLoadNextScheduleInfo(void)
{
    m_NextScheduleHasbeenLoaded = 0;
}

void Scheduler::LoadCurrentScheduleInfo(bool future)
{
    m_currentSchedulePlaylist.ScheduleEntryIndex = GetNextScheduleEntry(&m_currentSchedulePlaylist.weeklySecondIndex, future);
    if (m_currentSchedulePlaylist.ScheduleEntryIndex != SCHEDULE_INDEX_INVALID) {
        m_currentSchedulePlaylist.startWeeklySeconds = m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startEndSeconds[m_currentSchedulePlaylist.weeklySecondIndex].first;
        m_currentSchedulePlaylist.endWeeklySeconds = m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startEndSeconds[m_currentSchedulePlaylist.weeklySecondIndex].second;
        m_currentSchedulePlaylist.entry = m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex];
    }

    m_CurrentScheduleHasbeenLoaded = 1;
}

void Scheduler::LoadNextScheduleInfo(void)
{
    m_nextSchedulePlaylist.ScheduleEntryIndex = GetNextScheduleEntry(&m_nextSchedulePlaylist.weeklySecondIndex, true);
    m_NextScheduleHasbeenLoaded = 1;

    if (m_nextSchedulePlaylist.ScheduleEntryIndex != SCHEDULE_INDEX_INVALID) {
        char t[64];
        char p[64];
        
        m_nextSchedulePlaylist.startWeeklySeconds = m_Schedule[m_nextSchedulePlaylist.ScheduleEntryIndex].startEndSeconds[m_nextSchedulePlaylist.weeklySecondIndex].first;
        m_nextSchedulePlaylist.endWeeklySeconds = m_Schedule[m_nextSchedulePlaylist.ScheduleEntryIndex].startEndSeconds[m_nextSchedulePlaylist.weeklySecondIndex].second;
        
        GetNextPlaylistText(p);
        GetNextScheduleStartText(t);
        LogDebug(VB_SCHEDULE, "Next Scheduled Playlist is index %d: '%s' for %s\n", m_nextSchedulePlaylist.ScheduleEntryIndex, p, t);
        m_nextSchedulePlaylist.entry = m_Schedule[m_nextSchedulePlaylist.ScheduleEntryIndex];
    }
}

void Scheduler::GetSunInfo(int set, int moffset, int &hour, int &minute, int &second)
{
	std::string latStr = getSetting("Latitude");
	std::string lonStr = getSetting("Longitude");

	if ((latStr == "") || (lonStr == ""))
	{
		latStr = "38.938524";
		lonStr = "-104.600945";

		LogErr(VB_SCHEDULE, "Error, Latitude/Longitude not filled in, using Falcon, Colorado coordinates!\n");
	}

	std::string::size_type sz;
	double lat = std::stod(latStr, &sz);
	double lon = std::stod(lonStr, &sz);
	double sunOffset = 0;
	time_t currTime = time(NULL);
	struct tm utc;
	struct tm local;

	gmtime_r(&currTime, &utc);
	localtime_r(&currTime, &local);

	LogDebug(VB_SCHEDULE, "Lat/Lon: %.6f, %.6f\n", lat, lon);
	LogDebug(VB_SCHEDULE, "Today (UTC) is %02d/%02d/%04d, UTC offset is %d hours\n",
		utc.tm_mon + 1, utc.tm_mday, utc.tm_year + 1900, local.tm_gmtoff / 3600);

	SunSet sun(lat, lon, local.tm_gmtoff / 3600);
	sun.setCurrentDate(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);

	if (set)
		sunOffset = sun.calcSunset();
	else
		sunOffset = sun.calcSunrise();
    
    sunOffset += moffset;

    if (sunOffset < 0) {
        LogDebug(VB_SCHEDULE, "Sunrise calculated as before midnight last night, using 8AM.  Check your time zone to make sure it is valid.\n");
        hour = 8;
        minute = 0;
        second = 0;
        return;
    } else if (sunOffset >= (24 * 60 * 60)) {
        LogDebug(VB_SCHEDULE, "Sunrise calculated as after midnight tomorrow, using 8PM.  Check your time zone to make sure it is valid.\n");
        hour = 20;
        minute = 0;
        second = 0;
        return;
    }

	LogDebug(VB_SCHEDULE, "SunRise/Set Time Offset: %.2f minutes\n", sunOffset);
	hour = (int)sunOffset / 60;
	minute = (int)sunOffset % 60;
	second = (int)(((int)(sunOffset * 100) % 100) * 0.01 * 60);

	if (set)
		LogDebug(VB_SCHEDULE, "Sunset is at %02d:%02d:%02d\n", hour, minute, second);
	else
		LogDebug(VB_SCHEDULE, "Sunrise is at %02d:%02d:%02d\n", hour, minute, second);
}


void Scheduler::SetScheduleEntrysWeeklyStartAndEndSeconds(ScheduleEntry *entry)
{
	if (entry->dayIndex & INX_DAY_MASK) {
		if (entry->dayIndex & INX_DAY_MASK_SUNDAY) {
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SUN,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SUN,entry->endHour,entry->endMinute,entry->endSecond));
		}

		if (entry->dayIndex & INX_DAY_MASK_MONDAY) {
			entry->pushStartEndTimes(GetWeeklySeconds(INX_MON,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_MON,entry->endHour,entry->endMinute,entry->endSecond));
		}

		if (entry->dayIndex & INX_DAY_MASK_TUESDAY) {
			entry->pushStartEndTimes(GetWeeklySeconds(INX_TUE,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_TUE,entry->endHour,entry->endMinute,entry->endSecond));
		}

		if (entry->dayIndex & INX_DAY_MASK_WEDNESDAY) {
			entry->pushStartEndTimes(GetWeeklySeconds(INX_WED,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_WED,entry->endHour,entry->endMinute,entry->endSecond));
		}

		if (entry->dayIndex & INX_DAY_MASK_THURSDAY) {
			entry->pushStartEndTimes(GetWeeklySeconds(INX_THU,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_THU,entry->endHour,entry->endMinute,entry->endSecond));
		}

		if (entry->dayIndex & INX_DAY_MASK_FRIDAY) {
			entry->pushStartEndTimes(GetWeeklySeconds(INX_FRI,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_FRI,entry->endHour,entry->endMinute,entry->endSecond));
		}

		if (entry->dayIndex & INX_DAY_MASK_SATURDAY) {
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SAT,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SAT,entry->endHour,entry->endMinute,entry->endSecond));
		}
		return;
    }

	// Some variables needed for odd/even day calculations
    struct std::tm FPPEpoch = {0,0,0,15,6,113}; //2013-07-15
    std::time_t FPPEpochTimeT = std::mktime(&FPPEpoch);
    std::time_t currTime = std::time(nullptr);
    double difference = std::difftime(currTime, FPPEpochTimeT) / (60 * 60 * 24);
	int daysSince = difference;
	int oddSunday = 0;
	int i = 0;

	struct tm now;
	localtime_r(&currTime, &now);

    if ((daysSince - now.tm_wday) % 2) {
		oddSunday = 1; // This past Sunday was an odd day
    }

	switch(entry->dayIndex) {
		case INX_SUN:
		case INX_MON:
		case INX_TUE:
		case INX_WED:
		case INX_THU:
		case INX_FRI:
        case INX_SAT:
            entry->pushStartEndTimes(GetWeeklySeconds(entry->dayIndex,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(entry->dayIndex,entry->endHour,entry->endMinute,entry->endSecond));
            break;
        case INX_EVERYDAY:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SUN,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SUN,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_MON,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_MON,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_TUE,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_TUE,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_WED,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_WED,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_THU,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_THU,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_FRI,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_FRI,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SAT,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SAT,entry->endHour,entry->endMinute,entry->endSecond));
            break;
        case INX_WKDAYS:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_MON,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_MON,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_TUE,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_TUE,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_WED,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_WED,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_THU,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_THU,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_FRI,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_FRI,entry->endHour,entry->endMinute,entry->endSecond));
            break;
        case INX_WKEND:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SAT,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SAT,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SUN,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SUN,entry->endHour,entry->endMinute,entry->endSecond));
            break;
        case INX_M_W_F:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_MON,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_MON,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_WED,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_WED,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_FRI,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_FRI,entry->endHour,entry->endMinute,entry->endSecond));
            break;
        case INX_T_TH:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_TUE,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_TUE,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_THU,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_THU,entry->endHour,entry->endMinute,entry->endSecond));
            break;
		case INX_SUN_TO_THURS:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SUN,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SUN,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_MON,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_MON,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_TUE,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_TUE,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_WED,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_WED,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_THU,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_THU,entry->endHour,entry->endMinute,entry->endSecond));
			break;
		case INX_FRI_SAT:
            entry->pushStartEndTimes(GetWeeklySeconds(INX_FRI,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_FRI,entry->endHour,entry->endMinute,entry->endSecond));
            entry->pushStartEndTimes(GetWeeklySeconds(INX_SAT,entry->startHour,entry->startMinute,entry->startSecond),
                                     GetWeeklySeconds(INX_SAT,entry->endHour,entry->endMinute,entry->endSecond));
            break;
        case INX_ODD_DAY: // Odd days starting at FPP epoch (2013-07-15 according to 'git log')
            for (int dow = 0; dow < 7; dow++) {
                if (((dow < now.tm_wday) &&
                     (( oddSunday && ((dow % 2) == 1)) ||
                      (!oddSunday && ((dow % 2) == 0)))) ||
                    ((dow >= now.tm_wday) &&
                     (( oddSunday && ((dow % 2) == 0)) ||
                      (!oddSunday && ((dow % 2) == 1))))) {
                    entry->pushStartEndTimes(GetWeeklySeconds(dow,entry->startHour,entry->startMinute,entry->startSecond),
                                             GetWeeklySeconds(dow,entry->endHour,entry->endMinute,entry->endSecond));
                }
            }
            break;
        case INX_EVEN_DAY: // Even days starting at FPP epoch (2013-07-15 according to 'git log')
            for (int dow = 0; dow < 7; dow++) {
                if (((dow < now.tm_wday) &&
                     (( oddSunday && ((dow % 2) == 0)) ||
                      (!oddSunday && ((dow % 2) == 1)))) ||
                    ((dow >= now.tm_wday) &&
                     (( oddSunday && ((dow % 2) == 1)) ||
                      (!oddSunday && ((dow % 2) == 0))))) {
                    entry->pushStartEndTimes(GetWeeklySeconds(dow,entry->startHour,entry->startMinute,entry->startSecond),
                                             GetWeeklySeconds(dow,entry->endHour,entry->endMinute,entry->endSecond));
                }
            }
            break;
        default:
            break;
    }
}



void Scheduler::PlayListLoadCheck(void)
{
  time_t currTime = time(NULL);
  struct tm now;
  
  localtime_r(&currTime, &now);

  int nowWeeklySeconds = GetWeeklySeconds(now.tm_wday, now.tm_hour, now.tm_min, now.tm_sec);
  if (m_nowWeeklySeconds2 != nowWeeklySeconds)
  {
    m_nowWeeklySeconds2 = nowWeeklySeconds;

    int diff = m_currentSchedulePlaylist.startWeeklySeconds - nowWeeklySeconds;
    int displayDiff = 0;

    if (diff < -600) // assume the schedule is actually next week for display
      diff += (24 * 3600 * 7);

    // If current schedule starttime is in the past and the item is not set
    // for repeat, then reschedule.
    if ((diff < -1) &&
        (!m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].repeat))
        LoadCurrentScheduleInfo();

    // Convoluted code to print the countdown more frequently as we get closer
    if (((diff > 300) &&                  ((diff % 300) == 0)) ||
        ((diff >  60) && (diff <= 300) && ((diff %  60) == 0)) ||
        ((diff >  10) && (diff <=  60) && ((diff %  10) == 0)) ||
        (                (diff <=  10)))
    {
      displayDiff = diff;
    }

    if (m_currentSchedulePlaylist.startWeeklySeconds && displayDiff)
      LogDebug(VB_SCHEDULE, "NowSecs = %d, CurrStartSecs = %d (%d seconds away)\n",
        nowWeeklySeconds,m_currentSchedulePlaylist.startWeeklySeconds, displayDiff);

    if(nowWeeklySeconds == m_currentSchedulePlaylist.startWeeklySeconds)
    {
      m_NextScheduleHasbeenLoaded = 0;
      LogInfo(VB_SCHEDULE, "Schedule Entry: %02d:%02d:%02d - %02d:%02d:%02d - Starting Playlist %s for %d seconds\n",
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startHour,
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startMinute,
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startSecond,
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].endHour,
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].endMinute,
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].endSecond,
        m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].playlist.c_str(),
        m_currentSchedulePlaylist.endWeeklySeconds - m_currentSchedulePlaylist.startWeeklySeconds);
      LogDebug(VB_SCHEDULE, "NowSecs = %d, CurrStartSecs = %d, CurrEndSecs = %d\n",
        nowWeeklySeconds, m_currentSchedulePlaylist.startWeeklySeconds, m_currentSchedulePlaylist.endWeeklySeconds);

        if ((playlist->getPlaylistStatus() != FPP_STATUS_IDLE) && (!playlist->WasScheduled())) {
            while (playlist->getPlaylistStatus() != FPP_STATUS_IDLE) {
                playlist->StopNow(1);
            }
        }

      m_currentSchedulePlaylist.SetTimes(currTime, nowWeeklySeconds);

      m_forcedNextPlaylist = SCHEDULE_INDEX_INVALID;
      playlist->Play(m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].playlist.c_str(),
        0, m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].repeat, 1);
    }
  }
}

void Scheduler::PlayListStopCheck(void)
{
    time_t currTime = time(NULL);
    struct tm now;
  
    localtime_r(&currTime, &now);

    int nowWeeklySeconds = GetWeeklySeconds(now.tm_wday, now.tm_hour, now.tm_min, now.tm_sec);

    if (m_nowWeeklySeconds2 != nowWeeklySeconds) {
        m_nowWeeklySeconds2 = nowWeeklySeconds;

        int diff = m_currentSchedulePlaylist.endWeeklySeconds - nowWeeklySeconds;

        // This check for 1 second ago is a hack rather than a more invasive
        // patch to handle the race condition if we miss this check on the exact
        // second the schedule should be ending.  The odds of us missing 2 in a row
        // are much lower, so this will suffice for v1.0.
        int stopPlaying = 0;

        // If the current item crosses the Saturday-Sunday midnight boundary and
        //    it is now at or past the end time on Sunday.
        if ((m_currentSchedulePlaylist.endWeeklySeconds < (24 * 60 * 60)) &&
            (m_currentSchedulePlaylist.startWeeklySeconds >= (6 * 24 * 60 * 60))) {
            // Need to nest this if so the else if below is less complicated
            if ((nowWeeklySeconds >= m_currentSchedulePlaylist.endWeeklySeconds) &&
                (nowWeeklySeconds < m_currentSchedulePlaylist.startWeeklySeconds)) {
                stopPlaying = 1;
            }
            diff += 7*24*60*60;
        } else if (nowWeeklySeconds >= m_currentSchedulePlaylist.endWeeklySeconds) {
            // Not a Saturday-Sunday rollover so just check if we are now past the
            //     end time of the current item
            stopPlaying = 1;
        }
      
        if (m_nextSchedulePlaylist.ScheduleEntryIndex != SCHEDULE_INDEX_INVALID &&
            m_nextSchedulePlaylist.ScheduleEntryIndex < m_currentSchedulePlaylist.ScheduleEntryIndex)  {
            //next scheduled is higher priority and we are within its time slot
            if ((nowWeeklySeconds > m_nextSchedulePlaylist.startWeeklySeconds) &&
                (nowWeeklySeconds < m_nextSchedulePlaylist.endWeeklySeconds)) {
                stopPlaying = 1;
                diff = 0;
                m_forcedNextPlaylist = m_nextSchedulePlaylist.ScheduleEntryIndex;
                // we are going to stop, but we need to make sure the "next" playlist actually will start
            }
        }
        int displayDiff = 0;

        // Convoluted code to print the countdown more frequently as we get closer
        if (((diff > 300) &&                  ((diff % 300) == 0)) ||
            ((diff >  60) && (diff <= 300) && ((diff %  60) == 0)) ||
            ((diff >  10) && (diff <=  60) && ((diff %  10) == 0)) ||
            (                (diff <=  10)))
        {
            displayDiff = diff;
        }
        if (displayDiff)
            LogDebug(VB_SCHEDULE, "NowSecs = %d, CurrEndSecs = %d (%d seconds away)\n",
                     nowWeeklySeconds, m_currentSchedulePlaylist.endWeeklySeconds, displayDiff);


        if (stopPlaying) {
            LogInfo(VB_SCHEDULE, "Schedule Entry: %02d:%02d:%02d - %02d:%02d:%02d - Stopping Playlist %s\n",
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startHour,
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startMinute,
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].startSecond,
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].endHour,
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].endMinute,
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].endSecond,
                m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].stopType == 0 ? "Gracefully" :
                    m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].stopType == 1 ? "Now with Hard Stop" :
                    m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].stopType == 2 ? "Gracefully after this loop" : "");


            // If the end time has been modified to be earlier than the originally
            // scheduled end time, then use a force stop so the main loop doesn't
            // restart the playlist when calling CheckIfShouldBePlayingNow()
            int forceStop = 0;
            if (m_currentSchedulePlaylist.actualEndTime < m_currentSchedulePlaylist.scheduledEndTime) {
                forceStop = 1;
            }

            switch (m_Schedule[m_currentSchedulePlaylist.ScheduleEntryIndex].stopType) {
                case 0:
                    playlist->StopGracefully(forceStop);
                    break;
                case 2:
                    playlist->StopGracefully(forceStop, 1);
                    break;
                case 1:
                default:
                    while (playlist->getPlaylistStatus() != FPP_STATUS_IDLE) {
                        playlist->StopNow(forceStop);
                        LoadCurrentScheduleInfo(true);
                    }
                    break;
            }
        }
    }
}

void Scheduler::ConvertScheduleFile(void)
{
    if ((!FileExists(SCHEDULE_FILE)) &&
        (FileExists(SCHEDULE_FILE_CSV))) {
        FILE *fp;
        char buf[512];
        char *s;
        int scheduleEntryCount = 0;
        int day;
        Json::Value newSchedule(Json::arrayValue);

        LogInfo(VB_SCHEDULE, "Converting Schedule CSV to JSON\n");
        fp = fopen(SCHEDULE_FILE_CSV, "r");
        if (fp == NULL)
        {
            return;
        }

        while(fgets(buf, 512, fp) != NULL)
        {
            ScheduleEntry scheduleEntry;
            scheduleEntry.LoadFromString(buf);

            Json::Value e = scheduleEntry.GetJson();
            newSchedule.append(e);
        }

        fclose(fp);

        if (SaveJsonToFile(newSchedule, SCHEDULE_FILE))
            unlink(SCHEDULE_FILE_CSV);
    }
}

void Scheduler::LoadScheduleFromFile(void)
{
  LogDebug(VB_SCHEDULE, "Loading Schedule from %s\n", SCHEDULE_FILE);

  m_loadSchedule = false;
  m_lastLoadDate = GetCurrentDateInt();

  std::unique_lock<std::mutex> lock(m_scheduleLock);
  m_Schedule.clear();

  std::string playlistFile;

  Json::Value sch = LoadJsonFromFile(SCHEDULE_FILE);
  for (int i = 0; i < sch.size(); i++) {
	ScheduleEntry scheduleEntry;
	scheduleEntry.LoadFromJson(sch[i]);

    playlistFile = getPlaylistDirectory();
    playlistFile += "/";
    playlistFile += scheduleEntry.playlist + ".json";

    std::string warning = "Scheduled Playlist '";
    warning += scheduleEntry.playlist + "' does not exist";

    if (scheduleEntry.enabled && !FileExists(playlistFile)) {
        LogErr(VB_SCHEDULE, "ERROR: Scheduled Playlist '%s' does not exist\n",
            scheduleEntry.playlist.c_str());
        WarningHolder::AddWarning(warning);
        continue;
    } else {
        WarningHolder::RemoveWarning(warning);
    }

	// Check for sunrise/sunset flags
	if (scheduleEntry.startHour == 25)
		GetSunInfo( 0,
                    scheduleEntry.startTimeOffset,
					scheduleEntry.startHour,
					scheduleEntry.startMinute,
					scheduleEntry.startSecond);
	else if (scheduleEntry.startHour == 26)
		GetSunInfo( 1,
                    scheduleEntry.startTimeOffset,
					scheduleEntry.startHour,
					scheduleEntry.startMinute,
					scheduleEntry.startSecond);

	if (scheduleEntry.endHour == 25)
		GetSunInfo( 0,
                    scheduleEntry.endTimeOffset,
					scheduleEntry.endHour,
					scheduleEntry.endMinute,
					scheduleEntry.endSecond);
	else if (scheduleEntry.endHour == 26)
		GetSunInfo( 1,
                    scheduleEntry.endTimeOffset,
					scheduleEntry.endHour,
					scheduleEntry.endMinute,
					scheduleEntry.endSecond);

    // Set WeeklySecond start and end times
    SetScheduleEntrysWeeklyStartAndEndSeconds(&scheduleEntry);
    m_Schedule.push_back(scheduleEntry);
  }

  lock.unlock();

  return;
}

void Scheduler::SchedulePrint(void)
{
  int i=0;
  char stopTypes[4] = "GHL";

  LogInfo(VB_SCHEDULE, "Current Schedule: (Status: '+' = Enabled, '-' = Disabled, '!' = Outside Date Range, '*' = Repeat, Stop (G)raceful/(L)oop/(H)ard\n");
  LogInfo(VB_SCHEDULE, "Stat Start & End Dates       Days         Start & End Times   Playlist\n");
  LogInfo(VB_SCHEDULE, "---- ----------------------- ------------ ------------------- ---------------------------------------------\n");
  for (i = 0; i < m_Schedule.size(); i++) {
    char dayStr[32];
    GetDayTextFromDayIndex(m_Schedule[i].dayIndex, dayStr);
    LogInfo(VB_SCHEDULE, "%c%c%c%c %04d-%02d-%02d - %04d-%02d-%02d %-12.12s %02d:%02d:%02d - %02d:%02d:%02d %s\n",
      m_Schedule[i].enabled ? '+': '-',
      CurrentDateInRange(m_Schedule[i].startDate, m_Schedule[i].endDate) ? ' ': '!',
      m_Schedule[i].repeat ? '*': ' ',
      stopTypes[m_Schedule[i].stopType],
      (int)(m_Schedule[i].startDate / 10000),
      (int)(m_Schedule[i].startDate % 10000 / 100),
      (int)(m_Schedule[i].startDate % 100),
      (int)(m_Schedule[i].endDate / 10000),
      (int)(m_Schedule[i].endDate % 10000 / 100),
      (int)(m_Schedule[i].endDate % 100),
      dayStr,
      m_Schedule[i].startHour,m_Schedule[i].startMinute,m_Schedule[i].startSecond,
      m_Schedule[i].endHour,m_Schedule[i].endMinute,m_Schedule[i].endSecond,
      m_Schedule[i].playlist.c_str());
  }

  LogDebug(VB_SCHEDULE, "//////////////////////////////////////////////////\n");
}

std::string Scheduler::GetWeekDayStrFromSeconds(int weeklySeconds)
{
    std::string result;

    if (weeklySeconds >= (6 * 24 * 60 * 60))
        result = "Sat";
    else if (weeklySeconds >= (5 * 24 * 60 * 60))
        result = "Fri";
    else if (weeklySeconds >= (4 * 24 * 60 * 60))
        result = "Thu";
    else if (weeklySeconds >= (3 * 24 * 60 * 60))
        result = "Wed";
    else if (weeklySeconds >= (2 * 24 * 60 * 60))
        result = "Tue";
    else if (weeklySeconds >= (1 * 24 * 60 * 60))
        result = "Mon";
    else
        result = "Sun";

    return result;
}


int Scheduler::GetWeeklySecondDifference(int weeklySeconds1, int weeklySeconds2)
{
  if (weeklySeconds1 < weeklySeconds2) {
      return weeklySeconds2 - weeklySeconds1;
  } else if (weeklySeconds1 > weeklySeconds2) {
      return (SECONDS_PER_WEEK - weeklySeconds1) + weeklySeconds2;
  }
  return 0;
}

int Scheduler::GetDayFromWeeklySeconds(int weeklySeconds)
{
  return (int)(weeklySeconds/SECONDS_PER_DAY);
}

int Scheduler::FindNextStartingScheduleIndex(void)
{
	int i = 0;
	int index = -1;
	int currentDate = GetCurrentDateInt();
	int schedDate = 99999999;
	int weeklySecondStart = 999999;
	for(i = 0; i < m_Schedule.size(); i++) {
		if ((m_Schedule[i].enabled) &&
			(m_Schedule[i].startDate >= currentDate) &&
			(m_Schedule[i].endDate < schedDate)) {
			index = i;
			schedDate = m_Schedule[i].endDate;
		}
	}

	return index;
}

void Scheduler::GetNextScheduleStartText(char * txt)
{
	if (!m_NextScheduleHasbeenLoaded)
		return;

	if (m_nextSchedulePlaylist.ScheduleEntryIndex >= 0) {
		GetScheduleEntryStartText(m_nextSchedulePlaylist.ScheduleEntryIndex,m_nextSchedulePlaylist.weeklySecondIndex,txt);
    } else {
		char dayText[16];
		int found = FindNextStartingScheduleIndex();
		if (found >= 0) {
			GetDayTextFromDayIndex(m_Schedule[found].dayIndex,dayText);
			sprintf(txt, "%04d-%02d-%02d @ %02d:%02d:%02d - (%s)\n",
				(int)(m_Schedule[found].startDate / 10000),
				(int)(m_Schedule[found].startDate % 10000 / 100),
				(int)(m_Schedule[found].startDate % 100),
				m_Schedule[found].startHour,
				m_Schedule[found].startMinute,
				m_Schedule[found].startSecond,
				dayText);
		}
	}
}

void Scheduler::GetNextPlaylistText(char * txt)
{
	if (!m_NextScheduleHasbeenLoaded)
		return;

	if (m_nextSchedulePlaylist.ScheduleEntryIndex >= 0)
	{
		strcpy(txt, m_Schedule[m_nextSchedulePlaylist.ScheduleEntryIndex].playlist.c_str());
	}
	else
	{
		int found = FindNextStartingScheduleIndex();

		if (found >= 0)
			strcpy(txt, m_Schedule[found].playlist.c_str());
	}
}

void Scheduler::GetScheduleEntryStartText(int index, int weeklySecondIndex, char * txt)
{
    char text[64];
    char dayText[16];
    int start = m_Schedule[index].startEndSeconds[weeklySecondIndex].first;
    if (index > m_currentSchedulePlaylist.ScheduleEntryIndex && m_currentSchedulePlaylist.ScheduleEntryIndex >= 0 && start < m_currentSchedulePlaylist.endWeeklySeconds) {
        start = m_currentSchedulePlaylist.endWeeklySeconds;
    }
    int dayIndex = GetDayFromWeeklySeconds(start);
    GetDayTextFromDayIndex(dayIndex,text);
    
    int seconds = start % 60;
    start /= 60;
    int minutes = start % 60;
    start /= 60;
    int hour = start % 24;

    if(m_Schedule[index].dayIndex > INX_SAT) {
        GetDayTextFromDayIndex(m_Schedule[index].dayIndex,dayText);
        sprintf(txt,"%s @ %02d:%02d:%02d - (%s)",
                text, hour, minutes, seconds, dayText);
    } else {
        sprintf(txt,"%s @ %02d:%02d:%02d",
                text, hour, minutes, seconds);
    }
}

void Scheduler::GetDayTextFromDayIndex(int index,char * txt)
{
	if (index & INX_DAY_MASK)
	{
		strcpy(txt,"Day Mask");
		return;
	}

	switch(index)
	{
		case 0:
			strcpy(txt,"Sunday");
			break;		
		case 1:
			strcpy(txt,"Monday");
			break;		
		case 2:
			strcpy(txt,"Tuesday");
			break;		
		case 3:
			strcpy(txt,"Wednesday");
			break;		
		case 4:
			strcpy(txt,"Thursday");
			break;		
		case 5:
			strcpy(txt,"Friday");
			break;		
		case 6:
			strcpy(txt,"Saturday");
			break;		
		case 7:
			strcpy(txt,"Everyday");
			break;	
		case 8:
			strcpy(txt,"Weekdays");
			break;	
		case 9:
			strcpy(txt,"Weekends");
			break;	
		case 10:
			strcpy(txt,"Mon/Wed/Fri");
			break;	
		case 11:
			strcpy(txt,"Tues-Thurs");
			break;	
		case 12:
			strcpy(txt,"Sun-Thurs");
			break;	
		case 13:
			strcpy(txt,"Fri/Sat");
			break;	
		case 14:
			strcpy(txt,"Odd Days");
			break;
		case 15:
			strcpy(txt,"Even Days");
			break;
		default:
			strcpy(txt, "Error\0");
			break;	
	}
}

Json::Value Scheduler::GetInfo(void)
{
    Json::Value result;

    if (m_NextScheduleHasbeenLoaded) {
        Json::Value np;
        char NextPlaylistStr[128] = "No playlist scheduled.";
        char NextScheduleStartText[64] = "";

        GetNextScheduleStartText(NextScheduleStartText);
        GetNextPlaylistText(NextPlaylistStr);

        np["playlistName"] = NextPlaylistStr;
        np["scheduledStartTime"] = 0;
        np["scheduledStartTimeStr"] = NextScheduleStartText;

        result["nextPlaylist"] = np;
    }

    if ((playlist->getPlaylistStatus() == FPP_STATUS_PLAYLIST_PLAYING) ||
        (playlist->getPlaylistStatus() == FPP_STATUS_STOPPING_GRACEFULLY) ||
        (playlist->getPlaylistStatus() == FPP_STATUS_STOPPING_GRACEFULLY_AFTER_LOOP)) {
        Json::Value cp;

        if (playlist->WasScheduled()) {
            char timeStr[13];

            struct tm ast;
            localtime_r(&m_currentSchedulePlaylist.actualStartTime, &ast);

            struct tm aet;
            localtime_r(&m_currentSchedulePlaylist.actualEndTime, &aet);

            time_t currTime = time(NULL);
            struct tm now;

            localtime_r(&currTime, &now);
            int nowWeeklySeconds = GetWeeklySeconds(now.tm_wday, now.tm_hour, now.tm_min, now.tm_sec);

            std::string todayStr = GetWeekDayStrFromSeconds(nowWeeklySeconds);
            std::string tmpDayStr;

            cp["currentTime"] = (Json::UInt64)currTime;

            tmpDayStr = GetWeekDayStrFromSeconds(m_currentSchedulePlaylist.startWeeklySeconds);
            if (tmpDayStr == todayStr)
                tmpDayStr = "";
            else
                tmpDayStr += " ";

            snprintf(timeStr, 13, "%s%02d:%02d:%02d", tmpDayStr.c_str(),
                m_currentSchedulePlaylist.entry.startHour, m_currentSchedulePlaylist.entry.startMinute,
                m_currentSchedulePlaylist.entry.startSecond);

            cp["scheduledStartTime"] = (Json::UInt64)m_currentSchedulePlaylist.scheduledStartTime;
            cp["scheduledStartTimeStr"] = timeStr;

            tmpDayStr = GetWeekDayStrFromSeconds(m_currentSchedulePlaylist.actualStartWeeklySeconds);
            if (tmpDayStr == todayStr)
                tmpDayStr = "";
            else
                tmpDayStr += " ";

            snprintf(timeStr, 13, "%s%02d:%02d:%02d", tmpDayStr.c_str(),
                ast.tm_hour, ast.tm_min, ast.tm_sec);

            cp["actualStartTime"] = (Json::UInt64)m_currentSchedulePlaylist.actualStartTime;
            cp["actualStartTimeStr"] = timeStr;

            tmpDayStr = GetWeekDayStrFromSeconds(m_currentSchedulePlaylist.endWeeklySeconds);
            if (tmpDayStr == todayStr)
                tmpDayStr = "";
            else
                tmpDayStr += " ";

            snprintf(timeStr, 13, "%s%02d:%02d:%02d", tmpDayStr.c_str(),
                m_currentSchedulePlaylist.entry.endHour, m_currentSchedulePlaylist.entry.endMinute,
                m_currentSchedulePlaylist.entry.endSecond);

            cp["scheduledEndTime"] = (Json::UInt64)m_currentSchedulePlaylist.scheduledEndTime;
            cp["scheduledEndTimeStr"] = timeStr;

            snprintf(timeStr, 13, "%s%02d:%02d:%02d", tmpDayStr.c_str(),
                aet.tm_hour, aet.tm_min, aet.tm_sec);

            cp["actualEndTime"] = (Json::UInt64)m_currentSchedulePlaylist.actualEndTime;
            cp["actualEndTimeStr"] = timeStr;

            int stopType = m_currentSchedulePlaylist.entry.stopType;
            cp["stopType"] = stopType;
            cp["stopTypeStr"] = stopType == 2 ? "Graceful Loop" : stopType == 1 ? "Hard" : "Graceful";
            cp["secondsRemaining"] = m_currentSchedulePlaylist.endWeeklySeconds - nowWeeklySeconds;

            cp["playlistName"] = m_currentSchedulePlaylist.entry.playlist.c_str();

            result["status"] = "playing";
        } else {
            cp["playlistName"] = playlist->GetPlaylistName();

            result["status"] = "manual";
        }

        result["currentPlaylist"] = cp;
    } else {
        result["status"] = "idle";
    }


    return result;
}

int Scheduler::ExtendRunningSchedule(int seconds)
{
    if ((playlist->getPlaylistStatus() != FPP_STATUS_PLAYLIST_PLAYING) &&
        (playlist->getPlaylistStatus() != FPP_STATUS_STOPPING_GRACEFULLY) &&
        (playlist->getPlaylistStatus() != FPP_STATUS_STOPPING_GRACEFULLY_AFTER_LOOP)) {
        LogInfo(VB_SCHEDULE, "Tried to extend a running playlist, but there is no playlist running.\n");
        return 0;
    }

    if (!playlist->WasScheduled()) {
        LogInfo(VB_SCHEDULE, "Tried to extend running playlist, but it was manually started.\n");
        return 0;
    }

    // UI should catch this, but also check here
    if ((seconds > (12 * 60 * 60)) ||
        (seconds < (-3 * 60 * 60))) {
        return 0;
    }

    if (seconds >= 0)
        LogDebug(VB_SCHEDULE, "Extending schedule by %d seconds\n", seconds);
    else
        LogDebug(VB_SCHEDULE, "Shortening schedule by %d seconds\n", seconds);

    std::unique_lock<std::mutex> lock(m_scheduleLock);

    m_currentSchedulePlaylist.endWeeklySeconds += seconds;

    if (m_currentSchedulePlaylist.endWeeklySeconds < 0)
        m_currentSchedulePlaylist.endWeeklySeconds += 7 * 24 * 60 * 60;

    if (m_currentSchedulePlaylist.endWeeklySeconds >= (7 * 24 * 60 * 60))
        m_currentSchedulePlaylist.endWeeklySeconds -= 7 * 24 * 60 * 60;

    m_currentSchedulePlaylist.actualEndTime += seconds;

    return 1;
}

class ScheduleCommand : public Command {
public:
    ScheduleCommand(const std::string &str, Scheduler *s) : Command(str), sched(s) {}
    virtual ~ScheduleCommand() {}

    Scheduler *sched;
};

class ExtendScheduleCommand : public ScheduleCommand {
public:
    ExtendScheduleCommand(Scheduler *s) : ScheduleCommand("Extend Schedule", s) {
        args.push_back(CommandArg("Seconds", "int", "Seconds").setRange(-12 * 60 * 60, 12 * 60 * 60).setDefaultValue("300").setAdjustable());
    }

    virtual std::unique_ptr<Command::Result> run(const std::vector<std::string> &args) override {
        if (args.size() != 1) {
            return std::make_unique<Command::ErrorResult>("Command needs 1 argument, found " + std::to_string(args.size()));
        }

        if (sched->ExtendRunningSchedule(std::stoi(args[0])))
            return std::make_unique<Command::Result>("Schedule Updated");

        return std::make_unique<Command::Result>("Error extending Schedule");
    }
};

void Scheduler::RegisterCommands() {
    CommandManager::INSTANCE.addCommand(new ExtendScheduleCommand(this));
}

