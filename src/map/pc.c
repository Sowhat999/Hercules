/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2012-2023 Hercules Dev Team
 * Copyright (C) Athena Dev Teams
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#define HERCULES_CORE

#include "config/core.h" // DBPATH, GP_BOUND_ITEMS, MAX_SPIRITBALL, RENEWAL, RENEWAL_ASPD, RENEWAL_CAST, RENEWAL_DROP, RENEWAL_EXP, SECURE_NPCTIMEOUT
#include "pc.h"

#include "map/atcommand.h" // get_atcommand_level()
#include "map/battle.h" // battle_config
#include "map/battleground.h"
#include "map/channel.h"
#include "map/chat.h"
#include "map/chrif.h"
#include "map/clan.h"
#include "map/clif.h"
#include "map/date.h" // is_day_of_*()
#include "map/duel.h"
#include "map/elemental.h"
#include "map/goldpc.h"
#include "map/guild.h" // guild-"search(), guild_request_info()
#include "map/homunculus.h"
#include "map/instance.h"
#include "map/intif.h"
#include "map/itemdb.h"
#include "map/log.h"
#include "map/mail.h"
#include "map/map.h"
#include "map/mercenary.h"
#include "map/messages.h"
#include "map/mob.h" // struct mob_data
#include "map/npc.h" // fake_nd
#include "map/party.h" // party-"search()
#include "map/path.h"
#include "map/pc_groups.h"
#include "map/pet.h" // pet_unlocktarget()
#include "map/quest.h"
#include "map/script.h" // script_config
#include "map/skill.h"
#include "map/status.h" // struct status_data
#include "map/storage.h"
#include "map/achievement.h"
#include "common/cbasetypes.h"
#include "common/conf.h"
#include "common/core.h" // get_svn_revision()
#include "common/HPM.h"
#include "common/memmgr.h"
#include "common/mmo.h" // NAME_LENGTH, MAX_CARTS, NEW_CARTS
#include "common/nullpo.h"
#include "common/random.h"
#include "common/showmsg.h"
#include "common/socket.h"
#include "common/sql.h"
#include "common/strlib.h" // safestrncpy()
#include "common/sysinfo.h"
#include "common/timer.h"
#include "common/utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct pc_interface pc_s;
struct pc_interface *pc;

static struct class_exp_tables exptables;

//Converts a class to its array index for CLASS_COUNT defined arrays.
//Note that it does not do a validity check for speed purposes, where parsing
//player input make sure to use a pc->db_checkid first!
static int pc_class2idx(int class)
{
	if (class >= JOB_NOVICE_HIGH) {
		class += - JOB_NOVICE_HIGH + JOB_MAX_BASIC;
	}
	Assert_ret(class >= 0 && class < CLASS_COUNT);
	return class;
}

/**
 * Creates a new dummy map session data.
 * Used when there is no real player attached, but it is
 * required to provide a session.
 * Caller must release dummy on its own when it's no longer needed.
 */
static struct map_session_data *pc_get_dummy_sd(void)
{
	struct map_session_data *dummy_sd;
	CREATE(dummy_sd, struct map_session_data, 1);
	dummy_sd->group = pcg->get_dummy_group(); // map_session_data.group is expected to be non-NULL at all times
	return dummy_sd;
}

/**
 * Sets player's group.
 * Caller should handle error (preferably display message and disconnect).
 * @param group_id Group ID
 * @return 1 on error, 0 on success
 */
static int pc_set_group(struct map_session_data *sd, int group_id)
{
	GroupSettings *group = pcg->id2group(group_id);
	nullpo_retr(1, sd);
	if (group == NULL)
		return 1;
	sd->group_id = group_id;
	sd->group = group;
	return 0;
}

/**
 * Checks if commands used by player should be logged.
 */
static bool pc_should_log_commands(struct map_session_data *sd)
{
	nullpo_retr(true, sd);
	return pcg->should_log_commands(sd->group);
}

static int pc_invincible_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);

	if (sd == NULL)
		return 1;

	if(sd->invincible_timer != tid){
		ShowError("invincible_timer %d != %d\n",sd->invincible_timer,tid);
		return 0;
	}
	sd->invincible_timer = INVALID_TIMER;
	skill->unit_move(&sd->bl,tick,1);

	return 0;
}

static void pc_setinvincibletimer(struct map_session_data *sd, int val)
{
	nullpo_retv(sd);

	val += map->list[sd->bl.m].invincible_time_inc;

	if( sd->invincible_timer != INVALID_TIMER )
		timer->delete(sd->invincible_timer,pc->invincible_timer);
	sd->invincible_timer = timer->add(timer->gettick()+val,pc->invincible_timer,sd->bl.id,0);
}

static void pc_delinvincibletimer(struct map_session_data *sd)
{
	nullpo_retv(sd);

	if( sd->invincible_timer != INVALID_TIMER )
	{
		timer->delete(sd->invincible_timer,pc->invincible_timer);
		sd->invincible_timer = INVALID_TIMER;
		skill->unit_move(&sd->bl,timer->gettick(),1);
	}
}

static int pc_spiritball_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);
	int i;

	if (sd == NULL)
		return 1;

	if( sd->spiritball <= 0 )
	{
		ShowError("pc_spiritball_timer: %d spiritball's available. (aid=%d cid=%d tid=%d)\n", sd->spiritball, sd->status.account_id, sd->status.char_id, tid);
		sd->spiritball = 0;
		return 0;
	}

	ARR_FIND(0, sd->spiritball, i, sd->spirit_timer[i] == tid);
	if( i == sd->spiritball )
	{
		ShowError("pc_spiritball_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->spiritball--;
	if( i != sd->spiritball )
		memmove(sd->spirit_timer+i, sd->spirit_timer+i+1, (sd->spiritball-i)*sizeof(int));
	sd->spirit_timer[sd->spiritball] = INVALID_TIMER;

	clif->spiritball(&sd->bl, BALL_TYPE_SPIRIT, AREA);

	return 0;
}

/**
 * Get the possible number of spiritball that a player can call.
 * @param sd the affected player structure
 * @param min the minimum number of spiritball regardless the level of MO_CALLSPIRITS
 * @retval total number of spiritball
 */
static int pc_getmaxspiritball(struct map_session_data *sd, int min)
{
	int result;

	nullpo_ret(sd);

	result = pc->checkskill(sd, MO_CALLSPIRITS);

	if ( min && result < min )
		result = min;
	else if ( sd->sc.data[SC_RAISINGDRAGON] )
		result += sd->sc.data[SC_RAISINGDRAGON]->val1;
	if ( result > MAX_SPIRITBALL )
		result = MAX_SPIRITBALL;
	return result;
}

static int pc_addspiritball(struct map_session_data *sd, int interval, int max)
{
	int tid, i;

	nullpo_ret(sd);

	if(max > MAX_SPIRITBALL)
		max = MAX_SPIRITBALL;
	if(sd->spiritball < 0)
		sd->spiritball = 0;

	if( sd->spiritball && sd->spiritball >= max ) {
		if(sd->spirit_timer[0] != INVALID_TIMER)
			timer->delete(sd->spirit_timer[0],pc->spiritball_timer);
		sd->spiritball--;
		if( sd->spiritball != 0 )
			memmove(sd->spirit_timer+0, sd->spirit_timer+1, (sd->spiritball)*sizeof(int));
		sd->spirit_timer[sd->spiritball] = INVALID_TIMER;
	}

	tid = timer->add(timer->gettick()+interval, pc->spiritball_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->spiritball, i, sd->spirit_timer[i] == INVALID_TIMER || DIFF_TICK(timer->get(tid)->tick, timer->get(sd->spirit_timer[i])->tick) < 0);
	if( i != sd->spiritball )
		memmove(sd->spirit_timer+i+1, sd->spirit_timer+i, (sd->spiritball-i)*sizeof(int));
	sd->spirit_timer[i] = tid;
	sd->spiritball++;
	pc->addspiritball_sub(sd);

	return 0;
}

static int pc_addspiritball_sub(struct map_session_data *sd)
{
	nullpo_ret(sd);
	if ((sd->job & MAPID_THIRDMASK) == MAPID_ROYAL_GUARD)
		clif->millenniumshield(&sd->bl,sd->spiritball);
	else
		clif->spiritball(&sd->bl, BALL_TYPE_SPIRIT, AREA);
	return 0;
}

static int pc_delspiritball(struct map_session_data *sd, int count, int type)
{
	int i;

	nullpo_ret(sd);

	if(sd->spiritball <= 0) {
		sd->spiritball = 0;
		return 0;
	}

	if(count <= 0)
		return 0;
	if(count > sd->spiritball)
		count = sd->spiritball;
	sd->spiritball -= count;
	if(count > MAX_SPIRITBALL)
		count = MAX_SPIRITBALL;

	for(i=0;i<count;i++) {
		if(sd->spirit_timer[i] != INVALID_TIMER) {
			timer->delete(sd->spirit_timer[i],pc->spiritball_timer);
			sd->spirit_timer[i] = INVALID_TIMER;
		}
	}
	for(i=count;i<MAX_SPIRITBALL;i++) {
		sd->spirit_timer[i-count] = sd->spirit_timer[i];
		sd->spirit_timer[i] = INVALID_TIMER;
	}

	if (!type) {
		pc->delspiritball_sub(sd);
	}
	return 0;
}

static int pc_delspiritball_sub(struct map_session_data *sd)
{
	nullpo_ret(sd);
	if ((sd->job & MAPID_THIRDMASK) == MAPID_ROYAL_GUARD)
		clif->millenniumshield(&sd->bl,sd->spiritball);
	else
		clif->spiritball(&sd->bl, BALL_TYPE_SPIRIT, AREA);
	return 0;
}

/**
 * @brief Adds a soulball to player
 * @param sd Player data
 * @param max Maximum amount of soulballs
 * @return (void)
 */
static void pc_addsoulball(struct map_session_data *sd, int max)
{
	nullpo_retv(sd);

	const struct status_change *sc = status->get_sc(&sd->bl);

	if (sc == NULL || sc->data[SC_SOULENERGY] == NULL) {
		sc_start(&sd->bl, &sd->bl, SC_SOULENERGY, 100, 0, skill->get_time2(SP_SOULCOLLECT, 1), 0);
		sd->soulball = 0;
	}

	if (max > MAX_SOUL_BALL)
		max = MAX_SOUL_BALL;

	sd->soulball = cap_value(sd->soulball + 1, 0, max);
	sc_start(&sd->bl, &sd->bl, SC_SOULENERGY, 100, sd->soulball, skill->get_time2(SP_SOULCOLLECT, 1), 0);
	clif->spiritball(&sd->bl, BALL_TYPE_SOUL, AREA);
}

/**
 * @brief Removes number of soulball from player
 * @param sd Player data
 * @param count Amount to remove
 * @param type true means doesn't give client effect
 * @return (void)
 */
static void pc_delsoulball(struct map_session_data *sd, int count, bool type)
{
	nullpo_retv(sd);

	if (count <= 0)
		return;

	struct status_change *sc = status->get_sc(&sd->bl);

	if (sd->soulball <= 0 || sc == NULL || sc->data[SC_SOULENERGY] == NULL) {
		sd->soulball = 0;
	} else {
		sd->soulball -= cap_value(count, 0, sd->soulball);
		if (sd->soulball == 0)
			status_change_end(&sd->bl, SC_SOULENERGY, INVALID_TIMER);
		else
			sc->data[SC_SOULENERGY]->val1 = sd->soulball;
	}

	if (type == 0)
		clif->spiritball(&sd->bl, BALL_TYPE_SOUL, AREA);
}

static int pc_check_banding(struct block_list *bl, va_list ap)
{
	int *c, *b_sd;
	struct block_list *src;
	const struct map_session_data *tsd;
	struct status_change *sc;

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_PC);
	tsd = BL_UCCAST(BL_PC, bl);

	nullpo_ret(src = va_arg(ap,struct block_list *));
	c = va_arg(ap,int *);
	b_sd = va_arg(ap, int *);

	if(pc_isdead(tsd))
		return 0;

	sc = status->get_sc(bl);

	if( sc && sc->data[SC_BANDING] )
	{
		b_sd[(*c)++] = tsd->bl.id;
		return 1;
	}

	return 0;
}
static int pc_banding(struct map_session_data *sd, uint16 skill_lv)
{
	int c;
	int b_sd[MAX_PARTY]; // In case of a full Royal Guard party.
	int i, j, hp, extra_hp = 0, tmp_qty = 0;
	struct map_session_data *bsd;
	struct status_change *sc;
	int range = skill->get_splash(LG_BANDING,skill_lv);

	nullpo_ret(sd);

	c = 0;
	memset(b_sd, 0, sizeof(b_sd));
	i = party->foreachsamemap(pc->check_banding,sd,range,&sd->bl,&c,&b_sd);

	if( c < 1 ) {
		//just recalc status no need to recalc hp
		if( (sc = status->get_sc(&sd->bl)) != NULL  && sc->data[SC_BANDING] ) {
			// No more Royal Guards in Banding found.
			sc->data[SC_BANDING]->val2 = 0; // Reset the counter
			status_calc_bl(&sd->bl, status->sc2scb_flag(SC_BANDING));
		}
		return 0;
	}

	//Add yourself
	hp = status_get_hp(&sd->bl);
	i++;

	// Get total HP of all Royal Guards in party.
	for( j = 0; j < i; j++ ) {
		bsd = map->id2sd(b_sd[j]);
		if( bsd != NULL )
			hp += status_get_hp(&bsd->bl);
	}

	// Set average HP.
	hp = hp / i;

	// If a Royal Guard have full HP, give more HP to others that haven't full HP.
	for (j = 0; j < i; j++) {
		int tmp_hp;
		bsd = map->id2sd(b_sd[j]);
		if (bsd != NULL && (tmp_hp = hp - status_get_max_hp(&bsd->bl)) > 0) {
			extra_hp += tmp_hp;
			tmp_qty++;
		}
	}

	if( extra_hp > 0 && tmp_qty > 0 )
		hp += extra_hp / tmp_qty;

	for( j = 0; j < i; j++ ) {
		bsd = map->id2sd(b_sd[j]);
		if( bsd != NULL ) {
			status->set_hp(&bsd->bl, hp, STATUS_HEAL_DEFAULT); // Set hp
			if( (sc = status->get_sc(&bsd->bl)) != NULL  && sc->data[SC_BANDING] ) {
				sc->data[SC_BANDING]->val2 = c; // Set the counter. It doesn't count your self.
				status_calc_bl(&bsd->bl, status->sc2scb_flag(SC_BANDING)); // Set atk and def.
			}
		}
	}

	return c;
}

/**
 * Increases a player's fame points and displays a notice to them.
 *
 * If the character's job class doesn't allow the specified rank type, nothing
 * happens and the request is ignored.
 *
 * @param sd    The target character.
 * @param type  The fame list type (@see enum fame_list_type).
 * @param count The amount of points to add.
 */
static void pc_addfame(struct map_session_data *sd, int ranktype, int count)
{
	nullpo_retv(sd);

	switch (ranktype) {
	case RANKTYPE_BLACKSMITH:
		if ((sd->job & MAPID_UPPERMASK) != MAPID_BLACKSMITH)
			return;
		break;
	case RANKTYPE_ALCHEMIST:
		if ((sd->job & MAPID_UPPERMASK) != MAPID_ALCHEMIST)
			return;
		break;
	case RANKTYPE_TAEKWON:
		if ((sd->job & MAPID_UPPERMASK) != MAPID_TAEKWON)
			return;
		break;
	case RANKTYPE_PK:
		// Not supported
		FALLTHROUGH
	default:
		Assert_retv(0);
	}

	sd->status.fame += count;
	if (sd->status.fame > MAX_FAME)
		sd->status.fame = MAX_FAME;

	clif->update_rankingpoint(sd, ranktype, count);
	chrif->updatefamelist(sd);
}

/**
 * Returns a character's rank in the specified fame list.
 *
 * @param char_id  The character ID.
 * @param ranktype The rank list type (@see enum fame_list_type).
 * @return The rank position (1-based index)
 * @retval 0 if the character isn't in the specified list.
 */
static int pc_fame_rank(int char_id, int ranktype)
{
	int i;

	switch (ranktype) {
	case RANKTYPE_BLACKSMITH:
		for (i = 0; i < MAX_FAME_LIST; i++) {
			if (pc->smith_fame_list[i].id == char_id)
				return i + 1;
		}
		break;
	case RANKTYPE_ALCHEMIST:
		for (i = 0; i < MAX_FAME_LIST; i++) {
			if (pc->chemist_fame_list[i].id == char_id)
				return i + 1;
		}
		break;
	case RANKTYPE_TAEKWON:
		for (i = 0; i < MAX_FAME_LIST; i++) {
			if (pc->taekwon_fame_list[i].id == char_id)
				return i + 1;
		}
		break;
	case RANKTYPE_PK: // Not implemented
		FALLTHROUGH
	default:
		Assert_ret(0);
	}

	return 0;
}

/**
 * Returns the appropriate fame list type for the given job.
 *
 * @param job_mapid The job (in MapID format)
 * @return the appropriate fame list type (@see enum fame_list_type).
 * @retval RANKTYPE_UNKNOWN if no appropriate type exists.
 */
static int pc_famelist_type(uint16 job_mapid)
{
	switch (job_mapid & MAPID_UPPERMASK) {
	case MAPID_BLACKSMITH:
		return RANKTYPE_BLACKSMITH;
	case MAPID_ALCHEMIST:
		return RANKTYPE_ALCHEMIST;
	case MAPID_TAEKWON:
		return RANKTYPE_TAEKWON;
	default:
		return RANKTYPE_UNKNOWN;
	}
}

static int pc_setrestartvalue(struct map_session_data *sd, int type)
{
	struct status_data *st, *bst;
	nullpo_ret(sd);

	bst = &sd->base_status;
	st = &sd->battle_status;

	if (type&1) {
		//Normal resurrection
		status->heal(&sd->bl, bst->hp, 0, STATUS_HEAL_FORCED | STATUS_HEAL_ALLOWREVIVE);
		if( st->sp < bst->sp )
			status->set_sp(&sd->bl, bst->sp, STATUS_HEAL_FORCED);
	} else { //Just for saving on the char-server (with values as if respawned)
		sd->status.hp = bst->hp;
		sd->status.sp = (st->sp < bst->sp) ? bst->sp : st->sp;
	}
	return 0;
}

/*==========================================
	Rental System
 *------------------------------------------*/
static int pc_inventory_rental_end(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);
	if( sd == NULL )
		return 0;
	if( tid != sd->rental_timer )
	{
		ShowError("pc_inventory_rental_end: invalid timer id.\n");
		return 0;
	}

	pc->inventory_rentals(sd);
	return 1;
}

static int pc_inventory_rental_clear(struct map_session_data *sd)
{
	nullpo_ret(sd);
	if( sd->rental_timer != INVALID_TIMER )
	{
		timer->delete(sd->rental_timer, pc->inventory_rental_end);
		sd->rental_timer = INVALID_TIMER;
	}

	return 1;
}
/* assumes i is valid (from default areas where it is called, it is) */
static void pc_rental_expire(struct map_session_data *sd, int i)
{
	nullpo_retv(sd);
	Assert_retv(i >= 0 && i < sd->status.inventorySize);

	clif->rental_expired(sd->fd, i, sd->status.inventory[i].nameid);
	pc->delitem(sd, i, sd->status.inventory[i].amount, 0, DELITEM_NORMAL, LOG_TYPE_RENTAL);
}
static void pc_inventory_rentals(struct map_session_data *sd)
{
	int c = 0;
	int64 expire_tick, next_tick = INT64_MAX;

	nullpo_retv(sd);
	for (int i = 0; i < sd->status.inventorySize; i++ )
	{ // Check for Rentals on Inventory
		if( sd->status.inventory[i].nameid == 0 )
			continue; // Nothing here
		if( sd->status.inventory[i].expire_time == 0 )
			continue;

		if( sd->status.inventory[i].expire_time <= time(NULL) ) {
			pc->rental_expire(sd,i);
		} else {
			expire_tick = (int64)(sd->status.inventory[i].expire_time - time(NULL)) * 1000;
			clif->rental_time(sd->fd, sd->status.inventory[i].nameid, (int)(expire_tick / 1000));
			next_tick = min(expire_tick, next_tick);
			c++;
		}
	}

	if( c > 0 ) // min(next_tick,3600000) 1 hour each timer to keep announcing to the owner, and to avoid a but with rental time > 15 days
		sd->rental_timer = timer->add(timer->gettick() + min(next_tick,3600000), pc->inventory_rental_end, sd->bl.id, 0);
	else
		sd->rental_timer = INVALID_TIMER;
}

static void pc_inventory_rental_add(struct map_session_data *sd, int seconds)
{
	int tick = seconds * 1000;

	if( sd == NULL )
		return;

	if( sd->rental_timer != INVALID_TIMER )
	{
		const struct TimerData * td;
		td = timer->get(sd->rental_timer);
		if( DIFF_TICK(td->tick, timer->gettick()) > tick )
		{ // Update Timer as this one ends first than the current one
			pc->inventory_rental_clear(sd);
			sd->rental_timer = timer->add(timer->gettick() + tick, pc->inventory_rental_end, sd->bl.id, 0);
		}
	}
	else
		sd->rental_timer = timer->add(timer->gettick() + min(tick,3600000), pc->inventory_rental_end, sd->bl.id, 0);
}

/*==========================================
 * prepares character for saving.
 *------------------------------------------*/
static int pc_makesavestatus(struct map_session_data *sd)
{
	nullpo_ret(sd);

	if(!battle_config.save_clothcolor)
		sd->status.clothes_color=0;

	if (!battle_config.save_body_style)
		sd->status.body = 0;


	//Only copy the Cart/Peco/Falcon options, the rest are handled via
	//status change load/saving. [Skotlex]
#ifdef NEW_CARTS
	sd->status.option = sd->sc.option&(OPTION_INVISIBLE|OPTION_FALCON|OPTION_RIDING|OPTION_DRAGON|OPTION_WUG|OPTION_WUGRIDER|OPTION_MADOGEAR);
#else
	sd->status.option = sd->sc.option&(OPTION_INVISIBLE|OPTION_CART|OPTION_FALCON|OPTION_RIDING|OPTION_DRAGON|OPTION_WUG|OPTION_WUGRIDER|OPTION_MADOGEAR);
#endif
	if (sd->sc.data[SC_JAILED]) { //When Jailed, do not move last point.
		if(pc_isdead(sd)){
			pc->setrestartvalue(sd,0);
		} else {
			sd->status.hp = sd->battle_status.hp;
			sd->status.sp = sd->battle_status.sp;
		}
		sd->status.last_point.map = sd->mapindex;
		sd->status.last_point.x = sd->bl.x;
		sd->status.last_point.y = sd->bl.y;
		return 0;
	}

	if(pc_isdead(sd)){
		pc->setrestartvalue(sd,0);
		memcpy(&sd->status.last_point,&sd->status.save_point,sizeof(sd->status.last_point));
	} else {
		sd->status.hp = sd->battle_status.hp;
		sd->status.sp = sd->battle_status.sp;
		sd->status.last_point.map = sd->mapindex;
		sd->status.last_point.x = sd->bl.x;
		sd->status.last_point.y = sd->bl.y;
	}

	if( ( map->list[sd->bl.m].flag.nosave && sd->state.autotrade != 2 ) || map->list[sd->bl.m].instance_id >= 0) {
		struct map_data *m=&map->list[sd->bl.m];
		if(m->save.map)
			memcpy(&sd->status.last_point,&m->save,sizeof(sd->status.last_point));
		else
			memcpy(&sd->status.last_point,&sd->status.save_point,sizeof(sd->status.last_point));
	}
	if( sd->status.last_point.map == 0 ) {
		sd->status.last_point.map = 1;
		sd->status.last_point.x = 0;
		sd->status.last_point.y = 0;
	}

	if( sd->status.save_point.map == 0 ) {
		sd->status.save_point.map = 1;
		sd->status.save_point.x = 0;
		sd->status.save_point.y = 0;
	}
	return 0;
}

/*==========================================
 * Off init ? Connection?
 *------------------------------------------*/
static int pc_setnewpc(struct map_session_data *sd, int account_id, int char_id, int login_id1, unsigned int client_tick, int sex, int fd)
{
	nullpo_ret(sd);

	sd->bl.id        = account_id;
	sd->status.account_id   = account_id;
	sd->status.char_id      = char_id;
	sd->status.sex   = sex;
	sd->login_id1    = login_id1;
	sd->login_id2    = 0; // at this point, we can not know the value :(
	sd->client_tick  = client_tick;
	sd->state.active = 0; //to be set to 1 after player is fully authed and loaded.
	sd->bl.type      = BL_PC;
	if (battle_config.prevent_logout_trigger & PLT_LOGIN)
		sd->canlog_tick = timer->gettick();
	//Required to prevent homunculus copuing a base speed of 0.
	sd->battle_status.speed = sd->base_status.speed = DEFAULT_WALK_SPEED;
	sd->state.warp_clean = 1;
	sd->catch_target_class = -1;
	return 0;
}

// [4144] probably pc_equippoint should be replaced to pc_item_equippoint
static int pc_equippoint(struct map_session_data *sd, int n)
{
	int ep = 0;

	nullpo_ret(sd);
	Assert_ret(n >= 0 && n < sd->status.inventorySize);

	if(!sd->inventory_data[n])
		return 0;

	if (!itemdb->isequip2(sd->inventory_data[n]))
		return 0; //Not equippable by players.

	ep = sd->inventory_data[n]->equip;
	if (sd->inventory_data[n]->subtype == W_DAGGER
	 || sd->inventory_data[n]->subtype == W_1HSWORD
	 || sd->inventory_data[n]->subtype == W_1HAXE
	) {
		if (pc->checkskill(sd,AS_LEFT) > 0
		 || (sd->job & MAPID_UPPERMASK) == MAPID_ASSASSIN
		 || (sd->job & MAPID_UPPERMASK) == MAPID_KAGEROUOBORO
		) {
			//Kagerou and Oboro can dual wield daggers. [Rytech]
			if( ep == EQP_HAND_R )
				return EQP_ARMS;
			if( ep == EQP_SHADOW_WEAPON )
				return EQP_SHADOW_ARMS;
		}
	}
	return ep;
}

static int pc_item_equippoint(struct map_session_data *sd, struct item_data *id)
{
	int ep = 0;

	nullpo_ret(sd);
	nullpo_ret(id);

	if (!itemdb->isequip2(id))
		return 0; //Not equippable by players.

	ep = id->equip;
	if (id->subtype == W_DAGGER ||
	    id->subtype == W_1HSWORD ||
	    id->subtype == W_1HAXE) {
		if (pc->checkskill(sd, AS_LEFT) > 0 ||
		    (sd->job & MAPID_UPPERMASK) == MAPID_ASSASSIN ||
		    (sd->job & MAPID_UPPERMASK) == MAPID_KAGEROUOBORO) {
			// Kagerou and Oboro can dual wield daggers. [Rytech]
			if (ep == EQP_HAND_R)
				return EQP_ARMS;
			if (ep == EQP_SHADOW_WEAPON)
				return EQP_SHADOW_ARMS;
		}
	}
	return ep;
}

static int pc_setinventorydata(struct map_session_data *sd)
{
	nullpo_ret(sd);

	for (int i = 0; i < sd->status.inventorySize; i++) {
		int id = sd->status.inventory[i].nameid;
		sd->inventory_data[i] = id?itemdb->search(id):NULL;
	}
	return 0;
}

static int pc_calcweapontype(struct map_session_data *sd)
{
	nullpo_ret(sd);

	// single-hand
	if(sd->weapontype2 == W_FIST) {
		sd->weapontype = sd->weapontype1;
		return 1;
	}
	if(sd->weapontype1 == W_FIST) {
		sd->weapontype = sd->weapontype2;
		return 1;
	}
	// dual-wield
	sd->weapontype = W_FIST;
	switch (sd->weapontype1){
		case W_DAGGER:
			switch (sd->weapontype2) {
				case W_DAGGER:  sd->weapontype = W_DOUBLE_DD; break;
				case W_1HSWORD: sd->weapontype = W_DOUBLE_DS; break;
				case W_1HAXE:   sd->weapontype = W_DOUBLE_DA; break;
			}
			break;
		case W_1HSWORD:
			switch (sd->weapontype2) {
				case W_DAGGER:  sd->weapontype = W_DOUBLE_DS; break;
				case W_1HSWORD: sd->weapontype = W_DOUBLE_SS; break;
				case W_1HAXE:   sd->weapontype = W_DOUBLE_SA; break;
			}
			break;
		case W_1HAXE:
			switch (sd->weapontype2) {
				case W_DAGGER:  sd->weapontype = W_DOUBLE_DA; break;
				case W_1HSWORD: sd->weapontype = W_DOUBLE_SA; break;
				case W_1HAXE:   sd->weapontype = W_DOUBLE_AA; break;
			}
	}
	// unknown, default to right hand type
	if (sd->weapontype == W_FIST)
		sd->weapontype = sd->weapontype1;

	return 2;
}

static int pc_setequipindex(struct map_session_data *sd)
{
	int i,j;

	nullpo_ret(sd);

	for(i=0;i<EQI_MAX;i++)
		sd->equip_index[i] = -1;

	for (i = 0; i < sd->status.inventorySize; i++) {
		if(sd->status.inventory[i].nameid <= 0)
			continue;
		if(sd->status.inventory[i].equip) {
			for(j=0;j<EQI_MAX;j++)
				if(sd->status.inventory[i].equip & pc->equip_pos[j])
					sd->equip_index[j] = i;

			if (sd->status.inventory[i].equip & EQP_HAND_R) {
				if (sd->inventory_data[i]) {
					sd->weapontype1 = sd->inventory_data[i]->subtype;
					sd->status.look.weapon = sd->inventory_data[i]->view_sprite;
				} else {
					sd->weapontype1 = W_FIST;
					sd->status.look.weapon = 0;
				}
			}

			if (sd->status.inventory[i].equip & EQP_HAND_L) {
				if (sd->inventory_data[i] != NULL) {
					if (sd->inventory_data[i]->type == IT_WEAPON)
						sd->weapontype2 = sd->inventory_data[i]->subtype;
					else
						sd->weapontype2 = W_FIST;
					if (sd->inventory_data[i]->type == IT_ARMOR)
						sd->has_shield = true;
					else
						sd->has_shield = false;
				} else {
					sd->weapontype2 = W_FIST;
					sd->has_shield = false;
				}
			}
		}
	}
	pc->calcweapontype(sd);

	return 0;
}

static bool pc_isequipped(struct map_session_data *sd, int nameid)
{
	int i, j;

	nullpo_retr(false, sd);
	for (i = 0; i < EQI_MAX; i++) {
		int index = sd->equip_index[i];
		if( index < 0 ) continue;

		if( i == EQI_HAND_R && sd->equip_index[EQI_HAND_L] == index ) continue;
		if( i == EQI_HEAD_MID && sd->equip_index[EQI_HEAD_LOW] == index ) continue;
		if( i == EQI_HEAD_TOP && (sd->equip_index[EQI_HEAD_MID] == index || sd->equip_index[EQI_HEAD_LOW] == index) ) continue;

		if( !sd->inventory_data[index] ) continue;

		if( sd->inventory_data[index]->nameid == nameid )
			return true;

		for( j = 0; j < MAX_SLOTS; j++ )
			if( sd->status.inventory[index].card[j] == nameid )
				return true;
	}

	return false;
}

static bool pc_can_Adopt(struct map_session_data *p1_sd, struct map_session_data *p2_sd, struct map_session_data *b_sd)
{
	if( !p1_sd || !p2_sd || !b_sd )
		return false;

	if( b_sd->status.father || b_sd->status.mother || b_sd->adopt_invite )
		return false; // already adopted baby / in adopt request

	if( !p1_sd->status.partner_id || !p1_sd->status.party_id || p1_sd->status.party_id != b_sd->status.party_id )
		return false; // You need to be married and in party with baby to adopt

	if( p1_sd->status.partner_id != p2_sd->status.char_id || p2_sd->status.partner_id != p1_sd->status.char_id )
		return false; // Not married, wrong married

	if( p2_sd->status.party_id != p1_sd->status.party_id )
		return false; // Both parents need to be in the same party

	// Parents need to have their ring equipped
	if( !pc->isequipped(p1_sd, WEDDING_RING_M) && !pc->isequipped(p1_sd, WEDDING_RING_F) )
		return false;

	if( !pc->isequipped(p2_sd, WEDDING_RING_M) && !pc->isequipped(p2_sd, WEDDING_RING_F) )
		return false;

	// Already adopted a baby
	if( p1_sd->status.child || p2_sd->status.child ) {
		clif->adopt_reply(p1_sd, 0);
		return false;
	}

	// Parents need at least lvl 70 to adopt
	if( p1_sd->status.base_level < 70 || p2_sd->status.base_level < 70 ) {
		clif->adopt_reply(p1_sd, 1);
		return false;
	}

	if( b_sd->status.partner_id ) {
		clif->adopt_reply(p1_sd, 2);
		return false;
	}

	if (!(b_sd->status.class >= JOB_NOVICE && b_sd->status.class <= JOB_THIEF) && b_sd->status.class != JOB_SUPER_NOVICE)
		return false;

	return true;
}

/*==========================================
 * Adoption Process
 *------------------------------------------*/
static bool pc_adoption(struct map_session_data *p1_sd, struct map_session_data *p2_sd, struct map_session_data *b_sd)
{
	int class, joblevel;
	uint64 jobexp;

	if( !pc->can_Adopt(p1_sd, p2_sd, b_sd) )
		return false;

	nullpo_retr(false, b_sd);
	// Preserve current job levels and progress
	joblevel = b_sd->status.job_level;
	jobexp = b_sd->status.job_exp;

	class = pc->mapid2jobid(b_sd->job | JOBL_BABY, b_sd->status.sex);
	if (class != -1 && !pc->jobchange(b_sd, class, 0)) {
		// Success, proceed to configure parents and baby skills
		p1_sd->status.child = b_sd->status.char_id;
		p2_sd->status.child = b_sd->status.char_id;
		b_sd->status.father = p1_sd->status.char_id;
		b_sd->status.mother = p2_sd->status.char_id;

		// Restore progress
		b_sd->status.job_level = joblevel;
		clif->updatestatus(b_sd, SP_JOBLEVEL);
		b_sd->status.job_exp = jobexp;
		clif->updatestatus(b_sd, SP_JOBEXP);

		// Baby Skills
		pc->skill(b_sd, WE_BABY, 1, SKILL_GRANT_PERMANENT);
		pc->skill(b_sd, WE_CALLPARENT, 1, SKILL_GRANT_PERMANENT);

		// Parents Skills
		pc->skill(p1_sd, WE_CALLBABY, 1, SKILL_GRANT_PERMANENT);
		pc->skill(p2_sd, WE_CALLBABY, 1, SKILL_GRANT_PERMANENT);

		// Achievements [Smokexyz/Hercules]
		achievement->validate_adopt(p1_sd, true); // Parent 1
		achievement->validate_adopt(p2_sd, true); // Parent 2
		achievement->validate_adopt(b_sd, false); // Baby

		return true;
	}

	return false; // Job Change Fail
}

/**
 * Checks if a character can equip an item.
 *
 * @param sd The related character.
 * @param n The item's inventory index.
 * @retval 0 Character can't equip the item.
 * @retval 1 Character can equip the item.
 *
 **/
static int pc_isequip(struct map_session_data *sd, int n)
{
	nullpo_ret(sd);
	Assert_ret(n >= 0 && n < sd->status.inventorySize);

	struct item_data *item = sd->inventory_data[n];

	if (item == NULL)
		return 0;

#if PACKETVER <= 20100707
	if (itemdb_is_shadowequip(item->equip) || itemdb_is_costumeequip(item->equip))
		return 0;
#endif

	if (pc_has_permission(sd, PC_PERM_USE_ALL_EQUIPMENT))
		return 1;

	if (item->elv != 0 && sd->status.base_level < item->elv) {
#if PACKETVER >= 20100525
		clif->msgtable(sd, MSG_CANNOT_EQUIP_ITEM_LEVEL);
#endif
		return 0;
	}

	if (item->elvmax != 0 && sd->status.base_level > item->elvmax) {
#if PACKETVER >= 20100525
		clif->msgtable(sd, MSG_CANNOT_EQUIP_ITEM_LEVEL);
#endif
		return 0;
	}

	if (item->sex != SEX_SERVER && sd->status.sex != item->sex)
		return 0;

	if ((item->equip & EQP_AMMO) != 0) {
		if (sd->state.active != 0 && !pc_iscarton(sd) && (sd->job & MAPID_THIRDMASK) == MAPID_GENETIC) { // Check if sc data is already loaded.
#if PACKETVER_RE_NUM >= 20090529 || PACKETVER_MAIN_NUM >= 20090603 || defined(PACKETVER_ZERO)
			clif->msgtable(sd, MSG_USESKILL_FAIL_CART);
#endif
			return 0;
		}

		if (!pc_ismadogear(sd) && (sd->job & MAPID_THIRDMASK) == MAPID_MECHANIC) {
#if PACKETVER_RE_NUM >= 20090226 || PACKETVER_MAIN_NUM >= 20090304 || defined(PACKETVER_ZERO)
			clif->msgtable(sd, MSG_USESKILL_FAIL_MADOGEAR);
#endif
			return 0;
		}
	}

	if ((battle_config.unequip_restricted_equipment & 1) != 0) {
		for (int i = 0; i < map->list[sd->bl.m].zone->disabled_items_count; i++)
			if (map->list[sd->bl.m].zone->disabled_items[i] == item->nameid)
				return 0;
	}

	if ((battle_config.unequip_restricted_equipment & 2) != 0 && !itemdb_isspecial(sd->status.inventory[n].card[0])) {
		for (int slot = 0; slot < item->slot; slot++)
			for (int i = 0; i < map->list[sd->bl.m].zone->disabled_items_count; i++)
				if (map->list[sd->bl.m].zone->disabled_items[i] == sd->status.inventory[n].card[slot])
					return 0;
	}

	if (sd->sc.count != 0) {
		if ((item->equip & EQP_ARMS) != 0 && item->type == IT_WEAPON && sd->sc.data[SC_NOEQUIPWEAPON] != NULL) // Also works with left-hand weapons. [DracoRPG]
			return 0;

		if ((item->equip & EQP_SHIELD) != 0 && item->type == IT_ARMOR && sd->sc.data[SC_NOEQUIPSHIELD] != NULL)
			return 0;

		if ((item->equip & EQP_ARMOR) != 0 && sd->sc.data[SC_NOEQUIPARMOR] != NULL)
			return 0;

		if ((item->equip & EQP_HEAD_TOP) != 0 && sd->sc.data[SC_NOEQUIPHELM] != NULL)
			return 0;

		if ((item->equip & EQP_ACC) != 0 && sd->sc.data[SC__STRIPACCESSARY] != NULL)
			return 0;

		if (item->equip != 0 && sd->sc.data[SC_KYOUGAKU] != NULL)
			return 0;

		if (sd->sc.data[SC_SOULLINK] != NULL && sd->sc.data[SC_SOULLINK]->val2 == SL_SUPERNOVICE) { // Spirit of Super Novice equip bonuses. [Skotlex]
			if (sd->status.base_level > 90 && (item->equip & EQP_HELM) != 0)
				return 1; // Can equip all helms.

			if (sd->status.base_level > 96 && (item->equip & EQP_ARMS) != 0 && item->type == IT_WEAPON) {
				switch (item->subtype) { // In weapons, the look determines type of weapon.
				case W_DAGGER: // Level 4 Knives are equippable.. this means all knives, I'd guess?
				case W_1HSWORD: // All 1H swords.
				case W_1HAXE: // All 1H axes.
				case W_MACE: // All 1H maces.
				case W_STAFF: // All 1H staffs.
					return 1;
				}
			}
		}
	}

	uint64 mask_job = 1ULL << (sd->job & MAPID_BASEMASK);
	uint64 mask_item = item->class_base[((sd->job & JOBL_2_1) != 0) ? 1 : (((sd->job & JOBL_2_2) != 0) ? 2 : 0)];

	if ((mask_job & mask_item) == 0) // Not equipable by class. [Skotlex]
		return 0;

	// Not usable by upper class. [Inkfish]
	while (1) {
		if ((item->class_upper & ITEMUPPER_NORMAL) != 0) {
			if ((sd->job & (JOBL_UPPER | JOBL_THIRD | JOBL_BABY)) == 0)
				break;
		}

		if ((item->class_upper & ITEMUPPER_UPPER) != 0) {
			if ((sd->job & (JOBL_UPPER | JOBL_THIRD)) != 0)
				break;
		}

		if ((item->class_upper & ITEMUPPER_BABY) != 0) {
			if ((sd->job & JOBL_BABY) != 0)
				break;
		}

		if ((item->class_upper & ITEMUPPER_THIRD) != 0) {
			if ((sd->job & JOBL_THIRD) != 0)
				break;
		}

		return 0;
	}

	return 1;
}

/*==========================================
 * No problem with the session id
 * set the status that has been sent from char server
 *------------------------------------------*/
static bool pc_authok(struct map_session_data *sd, int login_id2, time_t expiration_time, int group_id, const struct mmo_charstatus *st, bool changing_mapservers)
{
	int i;
	int64 tick = timer->gettick();
	uint32 ip;

	nullpo_retr(false, sd);
	ip = sockt->session[sd->fd]->client_addr;

	sd->login_id2 = login_id2;

	if (pc->set_group(sd, group_id) != 0) {
		ShowWarning("pc_authok: %s (AID:%d) logged in with unknown group id (%d)! kicking...\n",
			st->name, sd->status.account_id, group_id);
		clif->authfail_fd(sd->fd, 0);
		return false;
	}

	memcpy(&sd->status, st, sizeof(*st));
	memset(&sd->rodex, 0x0, sizeof(sd->rodex));
	VECTOR_INIT(sd->rodex.messages);
	VECTOR_INIT(sd->rodex.claim_list);

	if (st->sex != sd->status.sex) {
		clif->authfail_fd(sd->fd, 0);
		return false;
	}

	//Set the map-server used job id. [Skotlex]
	{
		int job = pc->jobid2mapid(sd->status.class);
		if (job == -1) {
			ShowError("pc_authok: Invalid class %d for player %s (%d:%d). Class was changed to novice.\n", sd->status.class, sd->status.name, sd->status.account_id, sd->status.char_id);
			sd->status.class = JOB_NOVICE;
			sd->job = MAPID_NOVICE;
		} else {
			sd->job = job;
		}
	}

	// Checks and fixes to character status data, that are required
	// in case of configuration change or stuff, which cannot be
	// checked on char-server.
	if( sd->status.hair < MIN_HAIR_STYLE || sd->status.hair > MAX_HAIR_STYLE ) {
		sd->status.hair = MIN_HAIR_STYLE;
	}
	if( sd->status.hair_color < MIN_HAIR_COLOR || sd->status.hair_color > MAX_HAIR_COLOR ) {
		sd->status.hair_color = MIN_HAIR_COLOR;
	}
	if( sd->status.clothes_color < MIN_CLOTH_COLOR || sd->status.clothes_color > MAX_CLOTH_COLOR ) {
		sd->status.clothes_color = MIN_CLOTH_COLOR;
	}
	if (sd->status.body < MIN_BODY_STYLE || sd->status.body > MAX_BODY_STYLE) {
		sd->status.body = MIN_BODY_STYLE;
	}

	//Initializations to null/0 unneeded since map_session_data was filled with 0 upon allocation.
	if(!sd->status.hp) pc_setdead(sd);
	sd->state.connect_new = 1;

	sd->followtimer = INVALID_TIMER; // [MouseJstr]
	sd->invincible_timer = INVALID_TIMER;
	sd->npc_timer_id = INVALID_TIMER;
	sd->pvp_timer = INVALID_TIMER;
	sd->fontcolor_tid = INVALID_TIMER;
	sd->expiration_tid = INVALID_TIMER;
	sd->macro_detect.timer = INVALID_TIMER;
	/**
	 * For the Secure NPC Timeout option (check config/Secure.h) [RR]
	 **/
#ifdef SECURE_NPCTIMEOUT
	/**
	 * Initialize to defaults/expected
	 **/
	sd->npc_idle_timer = INVALID_TIMER;
	sd->npc_idle_tick = tick;
	sd->npc_idle_type = NPCT_INPUT;
#endif

	sd->canuseitem_tick = tick;
	sd->canusecashfood_tick = tick;
	sd->canequip_tick = tick;
	sd->cantalk_tick = tick;
	sd->canskill_tick = tick;
	sd->cansendmail_tick = tick;
	sd->hchsysch_tick = tick;

	sd->idletime = sockt->last_tick;

	for(i = 0; i < MAX_SPIRITBALL; i++)
		sd->spirit_timer[i] = INVALID_TIMER;
	for(i = 0; i < ARRAYLENGTH(sd->autobonus); i++)
		sd->autobonus[i].active = INVALID_TIMER;
	for(i = 0; i < ARRAYLENGTH(sd->autobonus2); i++)
		sd->autobonus2[i].active = INVALID_TIMER;
	for(i = 0; i < ARRAYLENGTH(sd->autobonus3); i++)
		sd->autobonus3[i].active = INVALID_TIMER;

	if (battle_config.item_auto_get)
		sd->state.autoloot = 10000;

	if (battle_config.disp_experience)
		sd->state.showexp = 1;
	if (battle_config.disp_zeny)
		sd->state.showzeny = 1;

	if (!(battle_config.display_skill_fail&2))
		sd->state.showdelay = 1;

	pc->setinventorydata(sd);
	pc_setequipindex(sd);

	if( sd->status.option & OPTION_INVISIBLE && !pc->can_use_command(sd, "@hide") )
		sd->status.option &=~ OPTION_INVISIBLE;

	status->change_init(&sd->bl);

	sd->sc.option = sd->status.option; //This is the actual option used in battle.

	//Set here because we need the inventory data for weapon sprite parsing.
	status->set_viewdata(&sd->bl, sd->status.class);
	unit->dataset(&sd->bl);

	sd->guild_x = -1;
	sd->guild_y = -1;

	sd->disguise = -1;

	sd->instance = NULL;
	sd->instances = 0;

	sd->bg_queue.arena = NULL;
	sd->bg_queue.ready = 0;
	sd->bg_queue.client_has_bg_data = 0;
	sd->bg_queue.type = 0;

	VECTOR_INIT(sd->auto_cast); // Initialize auto-cast vector.
	VECTOR_INIT(sd->channels);
	VECTOR_INIT(sd->script_queues);
	VECTOR_INIT(sd->achievement); // Achievements [Smokexyz/Hercules]
	VECTOR_INIT(sd->storage.item); // initialize storage item vector.
	VECTOR_INIT(sd->hatEffectId);
	VECTOR_INIT(sd->agency_requests);

	sd->state.dialog = 0;

	sd->delayed_damage = 0;

	if (battle->bc->item_check != PCCHECKITEM_NONE) // Check and flag items for inspection.
		sd->itemcheck = (enum pc_checkitem_types) battle->bc->item_check;

	// Event Timers
	for( i = 0; i < MAX_EVENTTIMER; i++ )
		sd->eventtimer[i] = INVALID_TIMER;
	// Rental Timer
	sd->rental_timer = INVALID_TIMER;

	for( i = 0; i < MAX_PC_FEELHATE; i++ )
		sd->hate_mob[i] = -1;

	sd->quest_log = NULL;
	sd->num_quests = 0;
	sd->avail_quests = 0;
	sd->save_quest = false;

	sd->regs.vars = i64db_alloc(DB_OPT_BASE);
	sd->regs.arrays = NULL;
	sd->vars_dirty = false;
	sd->vars_ok = false;
	sd->vars_received = 0x0;

	sd->lang_id = map->default_lang_id;

	//warp player
	if ((i=pc->setpos(sd,sd->status.last_point.map, sd->status.last_point.x, sd->status.last_point.y, CLR_OUTSIGHT)) != 0) {
		ShowError ("Last_point_map %s - id %d not found (error code %d)\n", mapindex_id2name(sd->status.last_point.map), sd->status.last_point.map, i);

		// try warping to a default map instead (church graveyard)
		if (pc->setpos(sd, mapindex->name2id(MAP_PRONTERA), 273, 354, CLR_OUTSIGHT) != 0) {
			// if we fail again
			clif->authfail_fd(sd->fd, 0);
			return false;
		}
	} else if (map->getcell(map->mapindex2mapid(sd->status.last_point.map), &sd->bl, sd->status.last_point.x, sd->status.last_point.y, CELL_CHKNOPASS)) {
		//warp player stuck in invaild cell
		pc->setpos(sd,sd->status.last_point.map,0,0,CLR_OUTSIGHT);
	}

	clif->inventoryExpansionInfo(sd);
	clif->overweight_percent(sd);
	clif->authok(sd);

	//Prevent S. Novices from getting the no-death bonus just yet. [Skotlex]
	sd->die_counter=-1;

	//display login notice
	ShowInfo("'"CL_WHITE"%s"CL_RESET"' logged in."
	         " (AID/CID: '"CL_WHITE"%d/%d"CL_RESET"',"
	         " IP: '"CL_WHITE"%u.%u.%u.%u"CL_RESET"',"
	         " Group '"CL_WHITE"%d"CL_RESET"').\n",
	         sd->status.name, sd->status.account_id, sd->status.char_id,
	         CONVIP(ip), sd->group_id);

	// Send friends list
	clif->friendslist_send(sd);

	if( !changing_mapservers ) {

		if (battle_config.display_version == 1) {
			char buf[256];
			sprintf(buf, msg_sd(sd,1295), sysinfo->vcstype(), sysinfo->vcsrevision_src(), sysinfo->vcsrevision_scripts()); // %s revision '%s' (src) / '%s' (scripts)
			clif->message(sd->fd, buf);
		}

		if (expiration_time != 0) {
			sd->expiration_time = expiration_time;
		}

		/**
		 * Fixes login-without-aura glitch (the screen won't blink at this point, don't worry :P)
		 **/
		clif->changemap(sd,sd->bl.m,sd->bl.x,sd->bl.y);
	}

#ifdef GP_BOUND_ITEMS
	if( sd->status.party_id == 0 )
		pc->bound_clear(sd,IBT_PARTY);
#endif

	/* [Ind/Hercules] */
	sd->sc_display = NULL;
	sd->sc_display_count = 0;

	// Request all registries (auth is considered completed whence they arrive)
	intif->request_registry(sd,7);
	return true;
}

/*==========================================
 * Closes a connection because it failed to be authenticated from the char server.
 *------------------------------------------*/
static void pc_authfail(struct map_session_data *sd)
{
	nullpo_retv(sd);
	clif->authfail_fd(sd->fd, 0);
	return;
}

//Attempts to set a mob.
static int pc_set_hate_mob(struct map_session_data *sd, int pos, struct block_list *bl)
{
	int class_;
	if (!sd || !bl || pos < 0 || pos >= MAX_PC_FEELHATE)
		return 0;
	if (sd->hate_mob[pos] != -1) {
		//Can't change hate targets.
		clif->hate_info(sd, pos, sd->hate_mob[pos], 0); //Display current
		return 0;
	}

	class_ = status->get_class(bl);
	if (!pc->db_checkid(class_)) {
		unsigned int max_hp = status_get_max_hp(bl);
		if ((pos == 1 && max_hp < 6000) || (pos == 2 && max_hp < 20000))
			return 0;
		if (pos != status_get_size(bl))
			return 0; //Wrong size
	}
	sd->hate_mob[pos] = class_;
	pc_setglobalreg(sd,script->add_variable(pc->sg_info[pos].hate_var),class_+1);
	clif->hate_info(sd, pos, class_, 1);
	return 1;
}

/*==========================================
 * Invoked once after the char/account/account2 registry variables are received. [Skotlex]
 *------------------------------------------*/
static int pc_reg_received(struct map_session_data *sd)
{
	int i, idx = 0;

	nullpo_ret(sd);
	sd->vars_ok = true;

	sd->change_level_2nd = pc_readglobalreg(sd,script->add_variable("jobchange_level"));
	sd->change_level_3rd = pc_readglobalreg(sd,script->add_variable("jobchange_level_3rd"));
	sd->die_counter = pc_readglobalreg(sd,script->add_variable("PC_DIE_COUNTER"));

	// Cash shop
	sd->cashPoints = pc_readaccountreg(sd,script->add_variable("#CASHPOINTS"));
	sd->kafraPoints = pc_readaccountreg(sd,script->add_variable("#KAFRAPOINTS"));

	// Cooking Exp
	sd->cook_mastery = pc_readglobalreg(sd,script->add_variable("COOK_MASTERY"));

	if ((sd->job & MAPID_BASEMASK) == MAPID_TAEKWON) {
		// Better check for class rather than skill to prevent "skill resets" from unsetting this
		sd->mission_mobid = pc_readglobalreg(sd,script->add_variable("TK_MISSION_ID"));
		sd->mission_count = pc_readglobalreg(sd,script->add_variable("TK_MISSION_COUNT"));
	}

	//SG map and mob read [Komurka]
	for (i = 0; i < MAX_PC_FEELHATE; i++) {
		//for now - someone need to make reading from txt/sql
		int j = pc_readglobalreg(sd,script->add_variable(pc->sg_info[i].feel_var));
		if (j != 0) {
			sd->feel_map[i].index = j;
			sd->feel_map[i].m = map->mapindex2mapid(j);
		} else {
			sd->feel_map[i].index = 0;
			sd->feel_map[i].m = -1;
		}
		sd->hate_mob[i] = pc_readglobalreg(sd,script->add_variable(pc->sg_info[i].hate_var))-1;
	}

	if ((i = pc->checkskill(sd,RG_PLAGIARISM)) > 0) {
		sd->cloneskill_id = pc_readglobalreg(sd,script->add_variable("CLONE_SKILL"));
		if (sd->cloneskill_id > 0 && (idx = skill->get_index(sd->cloneskill_id)) > 0) {
			sd->status.skill[idx].id = sd->cloneskill_id;
			sd->status.skill[idx].lv = pc_readglobalreg(sd,script->add_variable("CLONE_SKILL_LV"));
			if (sd->status.skill[idx].lv > i)
				sd->status.skill[idx].lv = i;
			sd->status.skill[idx].flag = SKILL_FLAG_PLAGIARIZED;
		}
	}
	if ((i = pc->checkskill(sd,SC_REPRODUCE)) > 0) {
		sd->reproduceskill_id = pc_readglobalreg(sd,script->add_variable("REPRODUCE_SKILL"));
		if( sd->reproduceskill_id > 0 && (idx = skill->get_index(sd->reproduceskill_id)) > 0) {
			sd->status.skill[idx].id = sd->reproduceskill_id;
			sd->status.skill[idx].lv = pc_readglobalreg(sd,script->add_variable("REPRODUCE_SKILL_LV"));
			if( i < sd->status.skill[idx].lv)
				sd->status.skill[idx].lv = i;
			sd->status.skill[idx].flag = SKILL_FLAG_PLAGIARIZED;
		}
	}

	//Weird... maybe registries were reloaded?
	if (sd->state.active)
		return 0;
	sd->state.active = 1;

	if (sd->status.party_id)
		party->member_joined(sd);
	if (sd->status.guild_id)
		guild->member_joined(sd);

	if (sd->state.standalone == 0 && sd->state.autotrade == 0) { // prevents loading pets, homunculi, mercenaries or elementals if the character doesn't have a client attached
		if (sd->status.pet_id != 0)
			intif->request_petdata(sd->status.account_id, sd->status.char_id, sd->status.pet_id);
		if (sd->status.hom_id != 0)
			intif->homunculus_requestload(sd->status.account_id, sd->status.hom_id);
		if (sd->status.mer_id != 0)
			intif->mercenary_request(sd->status.mer_id, sd->status.char_id);
		if (sd->status.ele_id != 0)
			intif->elemental_request(sd->status.ele_id, sd->status.char_id);
	}

	map->addiddb(&sd->bl);
	map->delnickdb(sd->status.char_id, sd->status.name);
	if (!chrif->auth_finished(sd))
		ShowError("pc_reg_received: Failed to properly remove player %d:%d from logging db!\n", sd->status.account_id, sd->status.char_id);

	// Restore any cooldowns
	skill->cooldown_load(sd);
	pc->itemcd_do(sd, true);

	pc->load_combo(sd);

	status_calc_pc(sd,SCO_FIRST|SCO_FORCE);
	chrif->scdata_request(sd->status.account_id, sd->status.char_id);

	if (sd->status.clan_id)
		clan->member_online(sd, true);

	//Auth is fully okay, update last_login
	sd->status.last_login = time(NULL);

	// Storage Request
	intif->request_account_storage(sd);

	intif->Mail_requestinbox(sd->status.char_id, 0); // MAIL SYSTEM - Request Mail Inbox
	intif->request_questlog(sd);
	intif->rodex_checkhasnew(sd);

	if (sd->state.connect_new == 0 && sd->fd) { //Character already loaded map! Gotta trigger LoadEndAck manually.
		sd->state.connect_new = 1;
		clif->pLoadEndAck(sd->fd, sd);
	}

	if (pc_isinvisible(sd)) {
		sd->vd.class = INVISIBLE_CLASS;
		clif->message(sd->fd, msg_sd(sd,11)); // Invisible: On
		// decrement the number of pvp players on the map
		map->list[sd->bl.m].users_pvp--;

		if( map->list[sd->bl.m].flag.pvp && !map->list[sd->bl.m].flag.pvp_nocalcrank && sd->pvp_timer != INVALID_TIMER ) {// unregister the player for ranking
			timer->delete( sd->pvp_timer, pc->calc_pvprank_timer );
			sd->pvp_timer = INVALID_TIMER;
		}
		clif->changeoption(&sd->bl);
	}

	if( npc->motd ) /* [Ind/Hercules] */
		script->run(npc->motd->u.scr.script, 0, sd->bl.id, npc->fake_nd->bl.id);

	// Achievements [Smokexyz/Hercules]
	intif->achievements_request(sd);

	return 1;
}

static int pc_calc_skillpoint(struct map_session_data *sd)
{
	int  i,inf2,skill_point=0;

	nullpo_ret(sd);

	for (i = 1; i < MAX_SKILL_DB; i++) {
		int skill_lv = pc->checkskill2(sd,i);
		if (skill_lv > 0) {
			inf2 = skill->dbs->db[i].inf2;
			if((!(inf2&INF2_QUEST_SKILL) || battle_config.quest_skill_learn) &&
				!(inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL|INF2_GUILD_SKILL)) //Do not count wedding/link skills. [Skotlex]
				) {
				if(sd->status.skill[i].flag == SKILL_FLAG_PERMANENT)
					skill_point += skill_lv;
				else if(sd->status.skill[i].flag >= SKILL_FLAG_REPLACED_LV_0)
					skill_point += (sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0);
			}
		}
	}

	return skill_point;
}

static void pc_calc_skilltree_clear(struct map_session_data *sd)
{
	int i;

	nullpo_retv(sd);

	for (i = 0; i < MAX_SKILL_DB; i++) {
		if (sd->status.skill[i].flag != SKILL_FLAG_PLAGIARIZED && sd->status.skill[i].flag != SKILL_FLAG_PERM_GRANTED) //Don't touch these
			sd->status.skill[i].id = 0; //First clear skills.
		/* permanent skills that must be re-checked */
		if (sd->status.skill[i].flag == SKILL_FLAG_PERMANENT) {
			switch (skill->dbs->db[i].nameid) {
				case NV_TRICKDEAD:
					if ((sd->job & MAPID_UPPERMASK) != MAPID_NOVICE) {
						sd->status.skill[i].id = 0;
						sd->status.skill[i].lv = 0;
						sd->status.skill[i].flag = 0;
					}
					break;
			}
		}
	}
}

/*==========================================
 * Calculation of skill level.
 *------------------------------------------*/
static int pc_calc_skilltree(struct map_session_data *sd)
{
	nullpo_ret(sd);
	uint32 job = pc->calc_skilltree_normalize_job(sd);
	int class = pc->mapid2jobid(job, sd->status.sex);
	if (class == -1) {
		//Unable to normalize job??
		ShowError("pc_calc_skilltree: Unable to normalize job %u for character %s (%d:%d)\n", job, sd->status.name, sd->status.account_id, sd->status.char_id);
		return 1;
	}
	int classidx = pc->class2idx(class);

	pc->calc_skilltree_clear(sd);

	for (int i = 0; i < MAX_SKILL_DB; i++) {
		if (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY || sd->status.skill[i].flag >= SKILL_FLAG_REPLACED_LV_0) {
			// Restore original level of skills after deleting earned skills.
			sd->status.skill[i].lv = (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY) ? 0 : sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
		}
	}

	//Enable Bard/Dancer spirit linked skills.
	skill->add_bard_dancer_soullink_songs(sd);

	if( pc_has_permission(sd, PC_PERM_ALL_SKILL) ) {
		for (int i = 0; i < MAX_SKILL_DB; i++) {
			switch(skill->dbs->db[i].nameid) {
				/**
				 * Dummy skills must be added here otherwise they'll be displayed in the,
				 * skill tree and since they have no icons they'll give resource errors
				 **/
				case SM_SELFPROVOKE:
				case AB_DUPLELIGHT_MELEE:
				case AB_DUPLELIGHT_MAGIC:
				case WL_CHAINLIGHTNING_ATK:
				case WL_TETRAVORTEX_FIRE:
				case WL_TETRAVORTEX_WATER:
				case WL_TETRAVORTEX_WIND:
				case WL_TETRAVORTEX_GROUND:
				case WL_SUMMON_ATK_FIRE:
				case WL_SUMMON_ATK_WIND:
				case WL_SUMMON_ATK_WATER:
				case WL_SUMMON_ATK_GROUND:
				case LG_OVERBRAND_BRANDISH:
				case LG_OVERBRAND_PLUSATK:
				case RL_R_TRIP_PLUSATK:
					continue;
				default:
					break;
			}
			if( skill->dbs->db[i].inf2&(INF2_NPC_SKILL|INF2_GUILD_SKILL) )
				continue; //Only skills you can't have are npc/guild ones
			if( skill->dbs->db[i].max > 0 )
				sd->status.skill[i].id = skill->dbs->db[i].nameid;
		}
		return 0;
	}

	bool changed = false;
	do {
		changed = false;
		int id;
		for (int i = 0; i < MAX_SKILL_TREE && (id = pc->skill_tree[classidx][i].id) > 0; i++) {
			int idx = pc->skill_tree[classidx][i].idx;
			bool satisfied = true;
			if (sd->status.skill[idx].id > 0)
				continue; //Skill already known.

			if (!battle_config.skillfree) {
				int j;
				for (j = 0; j < VECTOR_LENGTH(pc->skill_tree[classidx][i].need); j++) {
					struct skill_tree_requirement *req = &VECTOR_INDEX(pc->skill_tree[classidx][i].need, j);
					int level;
					if (sd->status.skill[req->idx].id == 0
					 || sd->status.skill[req->idx].flag == SKILL_FLAG_TEMPORARY
					 || sd->status.skill[req->idx].flag == SKILL_FLAG_PLAGIARIZED)
						level = 0; //Not learned.
					else if (sd->status.skill[req->idx].flag >= SKILL_FLAG_REPLACED_LV_0) //Real learned level
						level = sd->status.skill[req->idx].flag - SKILL_FLAG_REPLACED_LV_0;
					else
						level = pc->checkskill2(sd, req->idx);
					if (level < req->lv) {
						satisfied = false;
						break;
					}
				}
				if (sd->status.job_level < (int)pc->skill_tree[classidx][i].joblv) {
					int jobid = pc->mapid2jobid(sd->job, sd->status.sex); // need to get its own skilltree
					if (jobid > -1) {
						if (!pc->skill_tree[pc->class2idx(jobid)][i].inherited)
							satisfied = false; // job level requirement wasn't satisfied
					} else {
						satisfied = false;
					}
				}
			}
			if (satisfied) {
				int inf2 = skill->dbs->db[idx].inf2;

				if(!sd->status.skill[idx].lv && (
					(inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
					inf2&INF2_WEDDING_SKILL ||
					(inf2&INF2_SPIRIT_SKILL && !sd->sc.data[SC_SOULLINK])
				))
					continue; //Cannot be learned via normal means. Note this check DOES allows raising already known skills.

				sd->status.skill[idx].id = id;

				if(inf2&INF2_SPIRIT_SKILL) { //Spirit skills cannot be learned, they will only show up on your tree when you get buffed.
					sd->status.skill[idx].lv = 1; // need to manually specify a skill level
					sd->status.skill[idx].flag = SKILL_FLAG_TEMPORARY; //So it is not saved, and tagged as a "bonus" skill.
				}
				changed = true; // skill list has changed, perform another pass
			}
		}
	} while (changed);

	pc->calc_skilltree_bonus(sd, classidx);

	return 0;
}

static void pc_calc_skilltree_bonus(struct map_session_data *sd, int classidx)
{
	int i;
	int id = 0;

	nullpo_retv(sd);
	Assert_retv(classidx >= 0 && classidx < CLASS_COUNT);

	//
	if (classidx > 0 && (sd->job & MAPID_UPPERMASK) == MAPID_TAEKWON
	 && sd->status.base_level >= 90 && sd->status.skill_point == 0
	 && pc->fame_rank(sd->status.char_id, RANKTYPE_TAEKWON) > 0) {
		/* Taekwon Ranker Bonus Skill Tree
		============================================
		- Grant All Taekwon Tree, but only as Bonus Skills in case they drop from ranking.
		- (c > 0) to avoid grant Novice Skill Tree in case of Skill Reset (need more logic)
		- (sd->status.skill_point == 0) to wait until all skill points are asigned to avoid problems with Job Change quest. */

		for (i = 0; i < MAX_SKILL_TREE && (id = pc->skill_tree[classidx][i].id) > 0; i++) {
			int idx = pc->skill_tree[classidx][i].idx;
			if( (skill->dbs->db[idx].inf2&(INF2_QUEST_SKILL|INF2_WEDDING_SKILL)) )
				continue; //Do not include Quest/Wedding skills.

			if( sd->status.skill[idx].id == 0 ) {
				sd->status.skill[idx].id = id;
				sd->status.skill[idx].flag = SKILL_FLAG_TEMPORARY; // So it is not saved, and tagged as a "bonus" skill.
			} else if( id != NV_BASIC ) {
				sd->status.skill[idx].flag = SKILL_FLAG_REPLACED_LV_0 + sd->status.skill[idx].lv; // Remember original level
			}

			sd->status.skill[idx].lv = skill->tree_get_max(id, sd->status.class);
		}
	}
}

//Checks if you can learn a new skill after having leveled up a skill.
static void pc_check_skilltree(struct map_session_data *sd, int skill_id)
{
	int i,id=0,flag;
	int c=0;

	if(battle_config.skillfree)
		return; //Function serves no purpose if this is set

	nullpo_retv(sd);
	i = pc->calc_skilltree_normalize_job(sd);
	c = pc->mapid2jobid(i, sd->status.sex);
	if (c == -1) { //Unable to normalize job??
		ShowError("pc_check_skilltree: Unable to normalize job %d for character %s (%d:%d)\n", i, sd->status.name, sd->status.account_id, sd->status.char_id);
		return;
	}
	c = pc->class2idx(c);
	do {
		flag = 0;
		for (i = 0; i < MAX_SKILL_TREE && (id = pc->skill_tree[c][i].id) > 0; i++) {
			int j, idx = pc->skill_tree[c][i].idx;
			bool satisfied = true;

			if (sd->status.skill[idx].id) //Already learned
				continue;

			for (j = 0; j < VECTOR_LENGTH(pc->skill_tree[c][i].need); j++) {
				struct skill_tree_requirement *req = &VECTOR_INDEX(pc->skill_tree[c][i].need, j);
				int level;
				if (sd->status.skill[req->idx].id == 0
				 || sd->status.skill[req->idx].flag == SKILL_FLAG_TEMPORARY
				 || sd->status.skill[req->idx].flag == SKILL_FLAG_PLAGIARIZED)
					level = 0; //Not learned.
				else if (sd->status.skill[req->idx].flag >= SKILL_FLAG_REPLACED_LV_0) //Real lerned level
					level = sd->status.skill[req->idx].flag - SKILL_FLAG_REPLACED_LV_0;
				else
					level = pc->checkskill2(sd,req->idx);
				if (level < req->lv) {
					satisfied = false;
					break;
				}
			}
			if (!satisfied)
				continue;

			if (sd->status.job_level < (int)pc->skill_tree[c][i].joblv) {
				int jobid = pc->mapid2jobid(sd->job, sd->status.sex); // need to get its own skilltree
				if (jobid > -1) {
					if (!pc->skill_tree[pc->class2idx(jobid)][i].inherited)
						continue;
				} else {
					continue;
				}
			}

			j = skill->dbs->db[idx].inf2;
			if( !sd->status.skill[idx].lv && (
				(j&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
				j&INF2_WEDDING_SKILL ||
				(j&INF2_SPIRIT_SKILL && !sd->sc.data[SC_SOULLINK])
			) )
				continue; //Cannot be learned via normal means.

			sd->status.skill[idx].id = id;

			flag = 1;
		}
	} while(flag);
}

// Make sure all the skills are in the correct condition
// before persisting to the backend.. [MouseJstr]
static int pc_clean_skilltree(struct map_session_data *sd)
{
	int i;
	nullpo_ret(sd);
	for (i = 0; i < MAX_SKILL_DB; i++) {
		if (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY || sd->status.skill[i].flag == SKILL_FLAG_PLAGIARIZED) {
			sd->status.skill[i].id = 0;
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = 0;
		} else if (sd->status.skill[i].flag >= SKILL_FLAG_REPLACED_LV_0) {
			sd->status.skill[i].lv = sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0;
			sd->status.skill[i].flag = 0;
		}
	}

	return 0;
}

static int pc_calc_skilltree_normalize_job(struct map_session_data *sd)
{
	int skill_point, novice_skills;
	uint16 job;

	nullpo_ret(sd);
	job = sd->job;
	if (!battle_config.skillup_limit || pc_has_permission(sd, PC_PERM_ALL_SKILL))
		return job;

	skill_point = pc->calc_skillpoint(sd);

	const struct class_exp_group *group = pc->dbs->class_exp_table[pc->class2idx(JOB_NOVICE)][CLASS_EXP_TABLE_JOB];
	nullpo_ret(group);
	novice_skills = group->max_level - 1;

	sd->sktree.second = sd->sktree.third = 0;

	if (skill_point < novice_skills && (sd->job & MAPID_BASEMASK) != MAPID_SUMMONER) {
		// limit 1st class and above to novice job levels
		job = MAPID_NOVICE;
	} else if ((sd->job & JOBL_2) != 0 && (sd->job & MAPID_UPPERMASK) != MAPID_SUPER_NOVICE) {
		// limit 2nd class and above to first class job levels (super novices are exempt)
		// regenerate change_level_2nd
		if (sd->change_level_2nd == 0) {
			if ((sd->job & JOBL_THIRD) != 0) {
				// if neither 2nd nor 3rd jobchange levels are known, we have to assume a default for 2nd
				if (sd->change_level_3rd == 0) {
					const struct class_exp_group *group2 = pc->dbs->class_exp_table[pc->class2idx(pc->mapid2jobid(sd->job & MAPID_UPPERMASK, sd->status.sex))][CLASS_EXP_TABLE_JOB];
					nullpo_ret(group2);
					sd->change_level_2nd = group2->max_level;
				} else {
					sd->change_level_2nd = 1 + skill_point + sd->status.skill_point
						- (sd->status.job_level - 1)
						- (sd->change_level_3rd - 1)
						- novice_skills;
				}
			} else {
				sd->change_level_2nd = 1 + skill_point + sd->status.skill_point
						- (sd->status.job_level - 1)
						- novice_skills;

			}

			pc_setglobalreg(sd, script->add_variable("jobchange_level"), sd->change_level_2nd);
		}

		if (skill_point < novice_skills + (sd->change_level_2nd - 1)) {
			job &= MAPID_BASEMASK;
			sd->sktree.second = ( novice_skills + (sd->change_level_2nd - 1) ) - skill_point;
		} else if ((sd->job & JOBL_THIRD) != 0) { // limit 3rd class to 2nd class/trans job levels
			// regenerate change_level_3rd
			if (sd->change_level_3rd == 0) {
					sd->change_level_3rd = 1 + skill_point + sd->status.skill_point
						- (sd->status.job_level - 1)
						- (sd->change_level_2nd - 1)
						- novice_skills;
					pc_setglobalreg(sd, script->add_variable("jobchange_level_3rd"), sd->change_level_3rd);
			}

			if (skill_point < novice_skills + (sd->change_level_2nd - 1) + (sd->change_level_3rd - 1)) {
				job &= MAPID_UPPERMASK;
				sd->sktree.third = (novice_skills + (sd->change_level_2nd - 1) + (sd->change_level_3rd - 1)) - skill_point;
			}
		}
	}

	// restore non-limiting flags
	job |= sd->job & (JOBL_UPPER|JOBL_BABY);

	return job;
}

/*==========================================
 * Updates the weight status
 *------------------------------------------
 * 1: overweight 50%
 * 2: overweight 90%
 * It's assumed that SC_WEIGHTOVER50 and SC_WEIGHTOVER90 are only started/stopped here.
 */
static int pc_updateweightstatus(struct map_session_data *sd)
{
	int old_overweight;
	int new_overweight;

	nullpo_retr(1, sd);

	old_overweight = (sd->sc.data[SC_WEIGHTOVER90]) ? 2 : (sd->sc.data[SC_WEIGHTOVER50]) ? 1 : 0;
	new_overweight = (pc_is90overweight(sd)) ? 2 : (pc_is50overweight(sd)) ? 1 : 0;

	if( old_overweight == new_overweight )
		return 0; // no change

	// stop old status change
	if( old_overweight == 1 )
		status_change_end(&sd->bl, SC_WEIGHTOVER50, INVALID_TIMER);
	else if( old_overweight == 2 )
		status_change_end(&sd->bl, SC_WEIGHTOVER90, INVALID_TIMER);

	// start new status change
	if( new_overweight == 1 )
		sc_start(NULL, &sd->bl, SC_WEIGHTOVER50, 100, 0, 0, 0);
	else if( new_overweight == 2 )
		sc_start(NULL, &sd->bl, SC_WEIGHTOVER90, 100, 0, 0, 0);

	// update overweight status
	sd->regen.state.overweight = new_overweight;

	return 0;
}

static int pc_disguise(struct map_session_data *sd, int class)
{
	nullpo_ret(sd);
	if (class == -1 && sd->disguise == -1)
		return 0;
	if (class >= 0 && sd->disguise == class)
		return 0;

	if (pc_isinvisible(sd)) { //Character is invisible. Stealth class-change. [Skotlex]
		sd->disguise = class; //viewdata is set on uncloaking.
		return 2;
	}

	if (sd->bl.prev != NULL) {
		if (class == -1 && sd->disguise == sd->status.class) {
			clif->clearunit_single(-sd->bl.id,CLR_OUTSIGHT,sd->fd);
		} else if (class != sd->status.class) {
			pc_stop_walking(sd, STOPWALKING_FLAG_NONE);
			clif->clearunit_area(&sd->bl, CLR_OUTSIGHT);
		}
	}

	if (class == -1) {
		sd->disguise = -1;
		class = sd->status.class;
	} else {
		sd->disguise = class;
	}

	status->set_viewdata(&sd->bl, class);
	clif->changeoption(&sd->bl);
	// We need to update the client so it knows that a costume is being used
	if( sd->sc.option&OPTION_COSTUME ) {
		clif->changelook(&sd->bl, LOOK_BASE, sd->vd.class);
		clif->changelook(&sd->bl,LOOK_WEAPON,0);
		clif->changelook(&sd->bl,LOOK_SHIELD,0);
		clif->changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
	}

	if (sd->bl.prev != NULL) {
		clif->spawn(&sd->bl);
		if (class == sd->status.class && pc_iscarton(sd)) {
			//It seems the cart info is lost on undisguise.
			clif->cartList(sd);
			clif->updatestatus(sd,SP_CARTINFO);
		}
		if (sd->chat_id != 0) {
			struct chat_data *cd = map->id2cd(sd->chat_id);

			if (cd != NULL)
				clif->dispchat(cd,0);
		}
	}
	return 1;
}

static int pc_bonus_autospell(struct s_autospell *spell, int max, short id, short lv, short rate, short flag, int card_id)
{
	int i;

	if( !rate )
		return 0;

	nullpo_ret(spell);
	Assert_ret(max <= 15);  // autospell array size
	for( i = 0; i < max && spell[i].id; i++ )
	{
		if( (spell[i].card_id == card_id || spell[i].rate < 0 || rate < 0) && spell[i].id == id && spell[i].lv == lv )
		{
			if( !battle_config.autospell_stacking && spell[i].rate > 0 && rate > 0 )
				return 0;
			rate += spell[i].rate;
			break;
		}
	}
	if (i == max) {
		ShowWarning("pc_bonus: Reached max (%d) number of autospells per character!\n", max);
		return 0;
	}
	spell[i].id = id;
	spell[i].lv = lv;
	spell[i].rate = rate;
	//Auto-update flag value.
	if (!(flag&BF_RANGEMASK)) flag|=BF_SHORT|BF_LONG; //No range defined? Use both.
	if (!(flag&BF_WEAPONMASK)) flag|=BF_WEAPON; //No attack type defined? Use weapon.
	if (!(flag&BF_SKILLMASK)) {
		if (flag&(BF_MAGIC|BF_MISC)) flag|=BF_SKILL; //These two would never trigger without BF_SKILL
		if (flag&BF_WEAPON) flag|=BF_NORMAL; //By default autospells should only trigger on normal weapon attacks.
	}
	spell[i].flag|= flag;
	spell[i].card_id = card_id;
	return 1;
}

static int pc_bonus_autospell_onskill(struct s_autospell *spell, int max, short src_skill, short id, short lv, short rate, int card_id)
{
	int i;

	if( !rate )
		return 0;

	nullpo_ret(spell);
	Assert_ret(max <= 15);  // autospell array size
	for( i = 0; i < max && spell[i].id; i++ )
	{
		;  // each autospell works independently
	}

	if( i == max )
	{
		ShowWarning("pc_bonus: Reached max (%d) number of autospells per character!\n", max);
		return 0;
	}

	spell[i].flag    = src_skill;
	spell[i].id      = id;
	spell[i].lv      = lv;
	spell[i].rate    = rate;
	spell[i].card_id = card_id;
	return 1;
}

/**
 * Adds an AddEff/AddEff2/AddEffWhenHit bonus to a character.
 *
 * @param effect     Effects array to append to.
 * @param max        Size of the effect array.
 * @param id         Effect ID (@see enum sc_type).
 * @param rate       Trigger rate.
 * @param arrow_rate Trigger rate modifier for ranged attacks (adds to the base rate).
 * @param flag       Trigger flags (@see enum auto_trigger_flag).
 * @param duration   Fixed (non-reducible) duration in ms. If 0, uses the default (reducible) duration of the given effect.
 * @retval 1 on success.
 * @retval 0 on failure.
 */
static int pc_bonus_addeff(struct s_addeffect *effect, int max, enum sc_type id, int16 rate, int16 arrow_rate, uint8 flag, uint16 duration)
{
	int i;

	nullpo_ret(effect);
	if (!(flag&(ATF_SHORT|ATF_LONG)))
		flag|=ATF_SHORT|ATF_LONG; //Default range: both
	if (!(flag&(ATF_TARGET|ATF_SELF)))
		flag|=ATF_TARGET; //Default target: enemy.
	if (!(flag&(ATF_WEAPON|ATF_MAGIC|ATF_MISC)))
		flag|=ATF_WEAPON; //Default type: weapon.

	for (i = 0; i < max && effect[i].flag; i++) {
		// Update existing effect if any.
		if (effect[i].id == id && effect[i].flag == flag && effect[i].duration == duration) {
			effect[i].rate += rate;
			effect[i].arrow_rate += arrow_rate;
			return 1;
		}
	}
	if (i == max) {
		ShowWarning("pc_bonus: Reached max (%d) number of add effects per character!\n", max);
		return 0;
	}
	effect[i].id = id;
	effect[i].rate = rate;
	effect[i].arrow_rate = arrow_rate;
	effect[i].flag = flag;
	effect[i].duration = duration;
	return 1;
}

static int pc_bonus_addeff_onskill(struct s_addeffectonskill *effect, int max, enum sc_type id, short rate, short skill_id, unsigned char target)
{
	int i;

	nullpo_ret(effect);
	for( i = 0; i < max && effect[i].skill; i++ ) {
		if( effect[i].id == id && effect[i].skill == skill_id && effect[i].target == target ) {
			effect[i].rate += rate;
			return 1;
		}
	}
	if( i == max ) {
		ShowWarning("pc_bonus: Reached max (%d) number of add effects on skill per character!\n", max);
		return 0;
	}
	effect[i].id = id;
	effect[i].rate = rate;
	effect[i].skill = skill_id;
	effect[i].target = target;
	return 1;
}

static int pc_bonus_item_drop(struct s_add_drop *drop, const short max, int id, bool is_group, int race_mask, int rate)
{
	int i;

	nullpo_ret(drop);
	Assert_ret(is_group || id > 0);
	//Apply config rate adjustment settings.
	if (rate >= 0) { //Absolute drop.
		if (battle_config.item_rate_adddrop != 100)
			rate = rate*battle_config.item_rate_adddrop/100;
		if (rate < battle_config.item_drop_adddrop_min)
			rate = battle_config.item_drop_adddrop_min;
		else if (rate > battle_config.item_drop_adddrop_max)
			rate = battle_config.item_drop_adddrop_max;
	} else { //Relative drop, max/min limits are applied at drop time.
		if (battle_config.item_rate_adddrop != 100)
			rate = rate*battle_config.item_rate_adddrop/100;
		if (rate > -1)
			rate = -1;
	}
	for (i = 0; i < max && (drop[i].id != 0 || drop[i].is_group); i++) {
		if (drop[i].id == id && race_mask != RCMASK_NONE) {
			drop[i].race |= race_mask;
			if (drop[i].rate > 0 && rate > 0) {
				//Both are absolute rates.
				if (drop[i].rate < rate)
					drop[i].rate = rate;
			} else if (drop[i].rate < 0 && rate < 0) {
				//Both are relative rates.
				if (drop[i].rate > rate)
					drop[i].rate = rate;
			} else if (rate < 0) //Give preference to relative rate.
					drop[i].rate = rate;
			return 1;
		}
	}
	if(i == max) {
		ShowWarning("pc_bonus: Reached max (%d) number of added drops per character!\n", max);
		return 0;
	}
	drop[i].id = id;
	drop[i].is_group = is_group;
	drop[i].race |= race_mask;
	drop[i].rate = rate;
	return 1;
}

static int pc_addautobonus(struct s_autobonus *bonus, char max, const char *bonus_script, short rate, unsigned int dur, short flag, const char *other_script, unsigned int pos, bool onskill)
{
	int i;

	nullpo_ret(bonus);
	nullpo_ret(bonus_script);
	ARR_FIND(0, max, i, bonus[i].rate == 0);
	if( i == max )
	{
		ShowWarning("pc_addautobonus: Reached max (%d) number of autobonus per character!\n", max);
		return 0;
	}

	if( !onskill )
	{
		if( !(flag&BF_RANGEMASK) )
			flag|=BF_SHORT|BF_LONG; //No range defined? Use both.
		if( !(flag&BF_WEAPONMASK) )
			flag|=BF_WEAPON; //No attack type defined? Use weapon.
		if( !(flag&BF_SKILLMASK) )
		{
			if( flag&(BF_MAGIC|BF_MISC) )
				flag|=BF_SKILL; //These two would never trigger without BF_SKILL
			if( flag&BF_WEAPON )
				flag|=BF_NORMAL|BF_SKILL;
		}
	}

	bonus[i].rate = rate;
	bonus[i].duration = dur;
	bonus[i].active = INVALID_TIMER;
	bonus[i].atk_type = flag;
	bonus[i].pos = pos;
	bonus[i].bonus_script = aStrdup(bonus_script);
	bonus[i].other_script = other_script?aStrdup(other_script):NULL;
	return 1;
}

static int pc_delautobonus(struct map_session_data *sd, struct s_autobonus *autobonus, char max, bool restore)
{
	int i;
	nullpo_ret(sd);
	nullpo_ret(autobonus);

	for( i = 0; i < max; i++ )
	{
		if( autobonus[i].active != INVALID_TIMER )
		{
			if( restore && sd->state.autobonus&autobonus[i].pos )
			{
				if( autobonus[i].bonus_script )
				{
					int j;
					ARR_FIND( 0, EQI_MAX, j, sd->equip_index[j] >= 0 && sd->status.inventory[sd->equip_index[j]].equip == autobonus[i].pos );
					if( j < EQI_MAX )
						script->run_autobonus(autobonus[i].bonus_script,sd->bl.id,sd->equip_index[j]);
				}
				continue;
			}
			else
			{ // Logout / Unequipped an item with an activated bonus
				timer->delete(autobonus[i].active,pc->endautobonus);
				autobonus[i].active = INVALID_TIMER;
			}
		}

		if( autobonus[i].bonus_script ) aFree(autobonus[i].bonus_script);
		if( autobonus[i].other_script ) aFree(autobonus[i].other_script);
		autobonus[i].bonus_script = autobonus[i].other_script = NULL;
		autobonus[i].rate = autobonus[i].atk_type = autobonus[i].duration = autobonus[i].pos = 0;
		autobonus[i].active = INVALID_TIMER;
	}

	return 0;
}

static int pc_exeautobonus(struct map_session_data *sd, struct s_autobonus *autobonus)
{
	nullpo_ret(sd);
	nullpo_ret(autobonus);

	if( autobonus->other_script )
	{
		int j;
		ARR_FIND( 0, EQI_MAX, j, sd->equip_index[j] >= 0 && sd->status.inventory[sd->equip_index[j]].equip == autobonus->pos );
		if( j < EQI_MAX )
			script->run_autobonus(autobonus->other_script,sd->bl.id,sd->equip_index[j]);
	}

	autobonus->active = timer->add(timer->gettick()+autobonus->duration, pc->endautobonus, sd->bl.id, (intptr_t)autobonus);
	sd->state.autobonus |= autobonus->pos;
	status_calc_pc(sd,SCO_NONE);

	return 0;
}

static int pc_endautobonus(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);
	struct s_autobonus *autobonus = (struct s_autobonus *)data;

	nullpo_ret(sd);
	nullpo_ret(autobonus);

	autobonus->active = INVALID_TIMER;
	sd->state.autobonus &= ~autobonus->pos;
	status_calc_pc(sd,SCO_NONE);
	return 0;
}

static void pc_bonus_addele(struct map_session_data *sd, unsigned char ele, short rate, short flag)
{
	int i;
	struct weapon_data *wd;

	nullpo_retv(sd);
	wd = (sd->state.lr_flag ? &sd->left_weapon : &sd->right_weapon);

	ARR_FIND(0, MAX_PC_BONUS, i, wd->addele2[i].rate == 0);

	if (i == MAX_PC_BONUS) {
		ShowWarning("pc_addele: Reached max (%d) possible bonuses for this player.\n", MAX_PC_BONUS);
		return;
	}

	if ((flag & BF_RANGEMASK) == 0)
		flag |= BF_SHORT | BF_LONG;
	if ((flag & BF_WEAPONMASK) == 0)
		flag |= BF_WEAPON;
	if ((flag & BF_SKILLMASK) == 0) {
		if ((flag & (BF_MAGIC | BF_MISC)) != 0)
			flag |= BF_SKILL;
		if ((flag & BF_WEAPON) != 0)
			flag |= BF_NORMAL | BF_SKILL;
	}

	wd->addele2[i].ele = ele;
	wd->addele2[i].rate = rate;
	wd->addele2[i].flag = flag;

	return;
}

static void pc_bonus_subele(struct map_session_data *sd, unsigned char ele, short rate, short flag)
{
	int i;

	nullpo_retv(sd);
	ARR_FIND(0, MAX_PC_BONUS, i, sd->subele2[i].rate == 0);

	if (i == MAX_PC_BONUS) {
		ShowWarning("pc_subele: Reached max (%d) possible bonuses for this player.\n", MAX_PC_BONUS);
		return;
	}

	if ((flag & BF_RANGEMASK) == 0)
		flag |= BF_SHORT | BF_LONG;
	if ((flag & BF_WEAPONMASK) == 0)
		flag |= BF_WEAPON;
	if ((flag & BF_SKILLMASK) == 0) {
		if ((flag & (BF_MAGIC | BF_MISC)) != 0)
			flag |= BF_SKILL;
		if ((flag & BF_WEAPON) != 0)
			flag |= BF_NORMAL | BF_SKILL;
	}

	sd->subele2[i].ele = ele;
	sd->subele2[i].rate = rate;
	sd->subele2[i].flag = flag;

	return;
}

/**
 * Loops through the fields in a race bitmask (enum RaceMask => enum Race)
 *
 * To be used in pc_bonus functions with races represented in array form.
 */
#define BONUS_FOREACH_RCARRAY_FROMMASK(loop_counter, mask) \
	for ((loop_counter) = RC_FORMLESS; (loop_counter) < RC_MAX; ++(loop_counter)) \
		if (((mask) & 1<<(loop_counter)) == RCMASK_NONE) { \
			continue; \
		} else

/*==========================================
 * Add a bonus(type) to player sd
 *------------------------------------------*/
static int pc_bonus(struct map_session_data *sd, int type, int val)
{
	struct status_data *bst;
	int bonus;
	int i;
	nullpo_ret(sd);

	bst = &sd->base_status;

	switch(type){
		case SP_STR:
		case SP_AGI:
		case SP_VIT:
		case SP_INT:
		case SP_DEX:
		case SP_LUK:
			if(sd->state.lr_flag != 2)
				sd->param_bonus[type-SP_STR]+=val;
			break;
		case SP_ATK1:
			if(!sd->state.lr_flag) {
				bonus = bst->rhw.atk + val;
				bst->rhw.atk = cap_value(bonus, 0, USHRT_MAX);
			}
			else if(sd->state.lr_flag == 1) {
				bonus = bst->lhw.atk + val;
				bst->lhw.atk =  cap_value(bonus, 0, USHRT_MAX);
			}
			break;
		case SP_ATK2:
			if(!sd->state.lr_flag) {
				bonus = bst->rhw.atk2 + val;
				bst->rhw.atk2 = cap_value(bonus, 0, USHRT_MAX);
			}
			else if(sd->state.lr_flag == 1) {
				bonus = bst->lhw.atk2 + val;
				bst->lhw.atk2 =  cap_value(bonus, 0, USHRT_MAX);
			}
			break;
		case SP_BASE_ATK:
			if(sd->state.lr_flag != 2) {
#ifdef RENEWAL
				bst->equip_atk += val;
#else
				bonus = bst->batk + val;
				bst->batk = cap_value(bonus, 0, USHRT_MAX);
#endif
			}
			break;
		case SP_DEF1:
			if(sd->state.lr_flag != 2) {
				bonus = bst->def + val;
	#ifdef RENEWAL
				bst->def = cap_value(bonus, SHRT_MIN, SHRT_MAX);
	#else
				bst->def = cap_value(bonus, CHAR_MIN, CHAR_MAX);
	#endif
			}
			break;
		case SP_DEF2:
			if(sd->state.lr_flag != 2) {
				bonus = bst->def2 + val;
				bst->def2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_MDEF1:
			if(sd->state.lr_flag != 2) {
				bonus = bst->mdef + val;
	#ifdef RENEWAL
				bst->mdef = cap_value(bonus, SHRT_MIN, SHRT_MAX);
	#else
				bst->mdef = cap_value(bonus, CHAR_MIN, CHAR_MAX);
	#endif
				if( sd->state.lr_flag == 3 ) {//Shield, used for royal guard
					sd->bonus.shieldmdef += bonus;
				}
			}
			break;
		case SP_MDEF2:
			if(sd->state.lr_flag != 2) {
				bonus = bst->mdef2 + val;
				bst->mdef2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_HIT:
			if(sd->state.lr_flag != 2) {
				bonus = bst->hit + val;
				bst->hit = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			} else
				sd->bonus.arrow_hit+=val;
			break;
		case SP_FLEE1:
			if(sd->state.lr_flag != 2) {
				bonus = bst->flee + val;
				bst->flee = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_FLEE2:
			if(sd->state.lr_flag != 2) {
				bonus = bst->flee2 + val*10;
				bst->flee2 = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			}
			break;
		case SP_CRITICAL:
			if(sd->state.lr_flag != 2) {
				bonus = bst->cri + val*10;
				bst->cri = cap_value(bonus, SHRT_MIN, SHRT_MAX);
			} else
				sd->bonus.arrow_cri += val*10;
			break;
		case SP_ATKELE:
			if(val >= ELE_MAX) {
				ShowError("pc_bonus: SP_ATKELE: Invalid element %d\n", val);
				break;
			}
			switch (sd->state.lr_flag) {
			case 2:
				switch (sd->weapontype) {
					case W_BOW:
					case W_REVOLVER:
					case W_RIFLE:
					case W_GATLING:
					case W_SHOTGUN:
					case W_GRENADE:
						//Become weapon element.
						bst->rhw.ele=val;
						break;
					default: //Become arrow element.
						sd->bonus.arrow_ele=val;
						break;
				}
				break;
			case 1:
				bst->lhw.ele=val;
				break;
			default:
				bst->rhw.ele=val;
				break;
			}
			break;
		case SP_DEFELE:
			if(val >= ELE_MAX) {
				ShowError("pc_bonus: SP_DEFELE: Invalid element %d\n", val);
				break;
			}
			if(sd->state.lr_flag != 2)
				bst->def_ele=val;
			break;
		case SP_MAXHP:
			if(sd->state.lr_flag == 2)
				break;
			val += (int)bst->max_hp;
			//Negative bonuses will underflow, this will be handled in status_calc_pc through casting
			//If this is called outside of status_calc_pc, you'd better pray they do not underflow and end with UINT_MAX max_hp.
			bst->max_hp = (unsigned int)val;
			break;
		case SP_MAXSP:
			if(sd->state.lr_flag == 2)
				break;
			val += (int)bst->max_sp;
			bst->max_sp = (unsigned int)val;
			break;
	#ifndef RENEWAL_CAST
		case SP_VARCASTRATE:
	#endif
		case SP_CASTRATE:
			if(sd->state.lr_flag != 2)
				sd->castrate+=val;
			break;
		case SP_MAXHPRATE:
			if(sd->state.lr_flag != 2)
				sd->hprate+=val;
			break;
		case SP_MAXSPRATE:
			if(sd->state.lr_flag != 2)
				sd->sprate+=val;
			break;
		case SP_SPRATE:
			if(sd->state.lr_flag != 2)
				sd->dsprate+=val;
			break;
		case SP_ATTACKRANGE:
			switch (sd->state.lr_flag) {
			case 2:
				switch (sd->weapontype) {
					case W_BOW:
					case W_REVOLVER:
					case W_RIFLE:
					case W_GATLING:
					case W_SHOTGUN:
					case W_GRENADE:
						bst->rhw.range += val;
				}
				break;
			case 1:
				bst->lhw.range += val;
				break;
			default:
				bst->rhw.range += val;
				break;
			}
			break;
		case SP_SPEED_RATE: //Non stackable increase
			if(sd->state.lr_flag != 2)
				sd->bonus.speed_rate = min(sd->bonus.speed_rate, -val);
			break;
		case SP_SPEED_ADDRATE: //Stackable increase
			if(sd->state.lr_flag != 2)
				sd->bonus.speed_add_rate -= val;
			break;
		case SP_ASPD: //Raw increase
			if(sd->state.lr_flag != 2)
				sd->bonus.aspd_add -= 10*val;
			break;
		case SP_ASPD_RATE: //Stackable increase - Made it linear as per rodatazone
			if(sd->state.lr_flag != 2)
	#ifndef RENEWAL_ASPD
				bst->aspd_rate -= 10*val;
	#else
				bst->aspd_rate2 += val;
	#endif
			break;
		case SP_HP_RECOV_RATE:
			if(sd->state.lr_flag != 2)
				sd->hprecov_rate += val;
			break;
		case SP_SP_RECOV_RATE:
			if(sd->state.lr_flag != 2)
				sd->sprecov_rate += val;
			break;
		case SP_CRITICAL_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.critical_def += val;
			break;
		case SP_NEAR_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.near_attack_def_rate += val;
			break;
		case SP_LONG_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.long_attack_def_rate += val;
			break;
		case SP_DOUBLE_RATE:
			if(sd->state.lr_flag == 0 && sd->bonus.double_rate < val)
				sd->bonus.double_rate = val;
			break;
		case SP_DOUBLE_ADD_RATE:
			if(sd->state.lr_flag == 0)
				sd->bonus.double_add_rate += val;
			break;
		case SP_MATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->matk_rate += val;
			break;
		case SP_IGNORE_DEF_ELE:
			if( (val >= ELE_MAX && val != ELE_ALL) || (val < ELE_NEUTRAL) ) {
				ShowError("pc_bonus: SP_IGNORE_DEF_ELE: Invalid element %d\n", val);
				break;
			}
			if ( val == ELE_ALL ) {
				for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ ) {
					if(!sd->state.lr_flag)
						sd->right_weapon.ignore_def_ele |= 1<<i;
					else if(sd->state.lr_flag == 1)
						sd->left_weapon.ignore_def_ele |= 1<<i;
				}
			} else {
				if(!sd->state.lr_flag)
					sd->right_weapon.ignore_def_ele |= 1<<val;
				else if(sd->state.lr_flag == 1)
					sd->left_weapon.ignore_def_ele |= 1<<val;
			}
			break;
		case SP_IGNORE_DEF_RACE:
		{
			uint32 race_mask = map->race_id2mask(val);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus: SP_IGNORE_DEF_RACE: Invalid Race (%d)\n", val);
				break;
			}
			if (!sd->state.lr_flag)
				sd->right_weapon.ignore_def_race |= race_mask;
			else if (sd->state.lr_flag == 1)
				sd->left_weapon.ignore_def_race |= race_mask;
		}
			break;
		case SP_ATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.atk_rate += val;
			break;
		case SP_MAGIC_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.magic_def_rate += val;
			break;
		case SP_MISC_ATK_DEF:
			if(sd->state.lr_flag != 2)
				sd->bonus.misc_def_rate += val;
			break;
		case SP_IGNORE_MDEF_RATE:
			if (sd->state.lr_flag != 2) {
				// Decomposed RC_ALL:
				sd->ignore_mdef[RC_NONBOSS] += val;
				sd->ignore_mdef[RC_BOSS] += val;
			}
			break;
		case SP_IGNORE_MDEF_ELE:
			if( (val >= ELE_MAX && val != ELE_ALL) || (val < ELE_NEUTRAL) ) {
				ShowError("pc_bonus: SP_IGNORE_MDEF_ELE: Invalid element %d\n", val);
				break;
			}
			if (sd->state.lr_flag != 2) {
				if ( val == ELE_ALL ) {
					for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ ) {
						sd->bonus.ignore_mdef_ele |= 1<<i;
					}
				} else {
					sd->bonus.ignore_mdef_ele |= 1<<val;
				}
			}
			break;
		case SP_IGNORE_MDEF_RACE:
		{
			uint32 race_mask = map->race_id2mask(val);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus: SP_IGNORE_MDEF_RACE: Invalid Race (%d)\n", val);
				break;
			}
			if (sd->state.lr_flag != 2) {
				sd->bonus.ignore_mdef_race |= race_mask;
			}
		}
			break;
		case SP_PERFECT_HIT_RATE:
			if(sd->state.lr_flag != 2 && sd->bonus.perfect_hit < val)
				sd->bonus.perfect_hit = val;
			break;
		case SP_PERFECT_HIT_ADD_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.perfect_hit_add += val;
			break;
		case SP_CRITICAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->critical_rate+=val;
			break;
		case SP_DEF_RATIO_ATK_ELE:
			if( (val >= ELE_MAX && val != ELE_ALL) || (val < ELE_NEUTRAL) ) {
				ShowError("pc_bonus: SP_DEF_RATIO_ATK_ELE: Invalid element %d\n", val);
				break;
			}
			if ( val == ELE_ALL ) {
				for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ ) {
					if(!sd->state.lr_flag)
						sd->right_weapon.def_ratio_atk_ele |= 1<<i;
					else if(sd->state.lr_flag == 1)
						sd->left_weapon.def_ratio_atk_ele |= 1<<i;
				}
			} else {
				if(!sd->state.lr_flag)
					sd->right_weapon.def_ratio_atk_ele |= 1<<val;
				else if(sd->state.lr_flag == 1)
					sd->left_weapon.def_ratio_atk_ele |= 1<<val;
			}
			break;
		case SP_DEF_RATIO_ATK_RACE:
		{
			uint32 race_mask = map->race_id2mask(val);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus: SP_DEF_RATIO_ATK_RACE: Invalid Race (%d)\n", val);
				break;
			}
			if (!sd->state.lr_flag)
				sd->right_weapon.def_ratio_atk_race |= race_mask;
			else if (sd->state.lr_flag == 1)
				sd->left_weapon.def_ratio_atk_race |= race_mask;
		}
			break;
		case SP_HIT_RATE:
			if(sd->state.lr_flag != 2)
				sd->hit_rate += val;
			break;
		case SP_FLEE_RATE:
			if(sd->state.lr_flag != 2)
				sd->flee_rate += val;
			break;
		case SP_FLEE2_RATE:
			if(sd->state.lr_flag != 2)
				sd->flee2_rate += val;
			break;
		case SP_DEF_RATE:
			if(sd->state.lr_flag != 2)
				sd->def_rate += val;
			break;
		case SP_DEF2_RATE:
			if(sd->state.lr_flag != 2)
				sd->def2_rate += val;
			break;
		case SP_MDEF_RATE:
			if(sd->state.lr_flag != 2)
				sd->mdef_rate += val;
			break;
		case SP_MDEF2_RATE:
			if(sd->state.lr_flag != 2)
				sd->mdef2_rate += val;
			break;
		case SP_RESTART_FULL_RECOVER:
			if(sd->state.lr_flag != 2)
				sd->special_state.restart_full_recover = 1;
			break;
		case SP_NO_CASTCANCEL:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_castcancel = 1;
			break;
		case SP_NO_CASTCANCEL2:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_castcancel2 = 1;
			break;
		case SP_NO_SIZEFIX:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_sizefix = 1;
			break;
		case SP_NO_MAGIC_DAMAGE:
			if(sd->state.lr_flag == 2)
				break;
			val+= sd->special_state.no_magic_damage;
			sd->special_state.no_magic_damage = cap_value(val,0,100);
			break;
		case SP_NO_WEAPON_DAMAGE:
			if(sd->state.lr_flag == 2)
				break;
			val+= sd->special_state.no_weapon_damage;
			sd->special_state.no_weapon_damage = cap_value(val,0,100);
			break;
		case SP_NO_MISC_DAMAGE:
			if(sd->state.lr_flag == 2)
				break;
			val+= sd->special_state.no_misc_damage;
			sd->special_state.no_misc_damage = cap_value(val,0,100);
			break;
		case SP_NO_GEMSTONE:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_gemstone = 1;
			break;
		case SP_INTRAVISION: // Maya Purple Card effect allowing to see Hiding/Cloaking people [DracoRPG]
			if(sd->state.lr_flag != 2) {
				sd->special_state.intravision = 1;
				clif->status_change(&sd->bl, status->get_sc_icon(SC_CLAIRVOYANCE), status->get_sc_relevant_bl_types(SC_CLAIRVOYANCE), 1, 0, 0, 0, 0);
			}
			break;
		case SP_NO_KNOCKBACK:
			if(sd->state.lr_flag != 2)
				sd->special_state.no_knockback = 1;
			break;
		case SP_SPLASH_RANGE:
			if(sd->bonus.splash_range < val)
				sd->bonus.splash_range = val;
			break;
		case SP_SPLASH_ADD_RANGE:
			sd->bonus.splash_add_range += val;
			break;
		case SP_SHORT_WEAPON_DAMAGE_RETURN:
			if(sd->state.lr_flag != 2)
				sd->bonus.short_weapon_damage_return += val;
			break;
		case SP_LONG_WEAPON_DAMAGE_RETURN:
			if(sd->state.lr_flag != 2)
				sd->bonus.long_weapon_damage_return += val;
			break;
		case SP_MAGIC_DAMAGE_RETURN: //AppleGirl Was Here
			if(sd->state.lr_flag != 2)
				sd->bonus.magic_damage_return += val;
			break;
		case SP_ALL_STATS: // [Valaris]
			if(sd->state.lr_flag!=2) {
				sd->param_bonus[SP_STR-SP_STR]+=val;
				sd->param_bonus[SP_AGI-SP_STR]+=val;
				sd->param_bonus[SP_VIT-SP_STR]+=val;
				sd->param_bonus[SP_INT-SP_STR]+=val;
				sd->param_bonus[SP_DEX-SP_STR]+=val;
				sd->param_bonus[SP_LUK-SP_STR]+=val;
			}
			break;
		case SP_AGI_VIT: // [Valaris]
			if(sd->state.lr_flag!=2) {
				sd->param_bonus[SP_AGI-SP_STR]+=val;
				sd->param_bonus[SP_VIT-SP_STR]+=val;
			}
			break;
		case SP_AGI_DEX_STR: // [Valaris]
			if(sd->state.lr_flag!=2) {
				sd->param_bonus[SP_AGI-SP_STR]+=val;
				sd->param_bonus[SP_DEX-SP_STR]+=val;
				sd->param_bonus[SP_STR-SP_STR]+=val;
			}
			break;
		case SP_PERFECT_HIDE: // [Valaris]
			if(sd->state.lr_flag!=2)
				sd->special_state.perfect_hiding=1;
			break;
		case SP_UNBREAKABLE:
			if(sd->state.lr_flag!=2)
				sd->bonus.unbreakable += val;
			break;
		case SP_UNBREAKABLE_WEAPON:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_WEAPON;
			break;
		case SP_UNBREAKABLE_ARMOR:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_ARMOR;
			break;
		case SP_UNBREAKABLE_HELM:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_HELM;
			break;
		case SP_UNBREAKABLE_SHIELD:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_SHIELD;
			break;
		case SP_UNBREAKABLE_GARMENT:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_GARMENT;
			break;
		case SP_UNBREAKABLE_SHOES:
			if(sd->state.lr_flag != 2)
				sd->bonus.unbreakable_equip |= EQP_SHOES;
			break;
		case SP_CLASSCHANGE: // [Valaris]
			if(sd->state.lr_flag !=2)
				sd->bonus.classchange=val;
			break;
		case SP_LONG_ATK_RATE:
			if(sd->state.lr_flag != 2) //[Lupus] it should stack, too. As any other cards rate bonuses
				sd->bonus.long_attack_atk_rate+=val;
			break;
		case SP_BREAK_WEAPON_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.break_weapon_rate+=val;
			break;
		case SP_BREAK_ARMOR_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.break_armor_rate+=val;
			break;
		case SP_ADD_STEAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_steal_rate+=val;
			break;
		case SP_DELAYRATE:
			if(sd->state.lr_flag != 2)
				sd->delayrate+=val;
			break;
		case SP_CRIT_ATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.crit_atk_rate += val;
			break;
		case SP_NO_REGEN:
			if(sd->state.lr_flag != 2)
				sd->regen.state.block|=val;
			break;
		case SP_UNSTRIPABLE_WEAPON:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_WEAPON;
			break;
		case SP_UNSTRIPABLE:
		case SP_UNSTRIPABLE_ARMOR:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_ARMOR;
			break;
		case SP_UNSTRIPABLE_HELM:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_HELM;
			break;
		case SP_UNSTRIPABLE_SHIELD:
			if(sd->state.lr_flag != 2)
				sd->bonus.unstripable_equip |= EQP_SHIELD;
			break;
		case SP_HP_DRAIN_VALUE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.hp_drain[RC_NONBOSS].value += val;
				sd->right_weapon.hp_drain[RC_BOSS].value += val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.hp_drain[RC_NONBOSS].value += val;
				sd->left_weapon.hp_drain[RC_BOSS].value += val;
			}
			break;
		case SP_SP_DRAIN_VALUE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.sp_drain[RC_NONBOSS].value += val;
				sd->right_weapon.sp_drain[RC_BOSS].value += val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.sp_drain[RC_NONBOSS].value += val;
				sd->left_weapon.sp_drain[RC_BOSS].value += val;
			}
			break;
		case SP_SP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.sp_gain_value += val;
			break;
		case SP_HP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.hp_gain_value += val;
			break;
		case SP_MAGIC_SP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.magic_sp_gain_value += val;
			break;
		case SP_MAGIC_HP_GAIN_VALUE:
			if(!sd->state.lr_flag)
				sd->bonus.magic_hp_gain_value += val;
			break;
		case SP_ADD_HEAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_heal_rate += val;
			break;
		case SP_ADD_HEAL2_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_heal2_rate += val;
			break;
		case SP_ADD_ITEM_HEAL_RATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.itemhealrate2 += val;
			break;
		case SP_EMATK:
			   if(sd->state.lr_flag != 2)
				   sd->bonus.ematk += val;
			   break;
		case SP_FIXCASTRATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.fixcastrate -= val;
			break;
		case SP_ADD_FIXEDCAST:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_fixcast += val;

			break;
	#ifdef RENEWAL_CAST
		case SP_VARCASTRATE:
			if(sd->state.lr_flag != 2)
				sd->bonus.varcastrate -= val;
			break;
		case SP_ADD_VARIABLECAST:
			if(sd->state.lr_flag != 2)
				sd->bonus.add_varcast += val;
			break;
	#endif
		case SP_ADD_MONSTER_DROP_CHAINITEM:
			if (sd->state.lr_flag != 2)
				pc->bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), val, true, map->race_id2mask(RC_ALL), 10000);
		break;
		case SP_ADDMAXWEIGHT:
			if (sd->state.lr_flag != 2)
				sd->max_weight += val;
			break;
		default:
			ShowWarning("pc_bonus: unknown type %d %d !\n",type,val);
			Assert_report(0);
			break;
	}
	return 0;
}

/*==========================================
 * Player bonus (type) with args type2 and val, called trough bonus2 (npc)
 *------------------------------------------*/
static int pc_bonus2(struct map_session_data *sd, int type, int type2, int val)
{
	int i;

	nullpo_ret(sd);

	switch(type){
		case SP_ADDELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus2: SP_ADDELE: Invalid element %d\n", type2);
				break;
			}
			if ( type2 == ELE_ALL ) {
				for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ ) {
					if ( !sd->state.lr_flag )
						sd->right_weapon.addele[i] += val;
					else if ( sd->state.lr_flag == 1 )
						sd->left_weapon.addele[i] += val;
					else if ( sd->state.lr_flag == 2 )
						sd->arrow_addele[i] += val;
				}
			} else {
				if(!sd->state.lr_flag)
					sd->right_weapon.addele[type2] += val;
				else if(sd->state.lr_flag == 1)
					sd->left_weapon.addele[type2] += val;
				else if(sd->state.lr_flag == 2)
					sd->arrow_addele[type2] += val;
			}
			break;
		case SP_ADDRACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_ADDRACE: Invalid Race (%d)\n", type2);
				break;
			}
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				if (sd->state.lr_flag == 0) {
					sd->right_weapon.addrace[i] += val;
				} else if (sd->state.lr_flag == 1) {
					sd->left_weapon.addrace[i] += val;
				} else if (sd->state.lr_flag == 2) {
					sd->arrow_addrace[i] += val;
				}
			}
		}
			break;
		case SP_ADDSIZE:
			if(!sd->state.lr_flag)
				sd->right_weapon.addsize[type2]+=val;
			else if(sd->state.lr_flag == 1)
				sd->left_weapon.addsize[type2]+=val;
			else if(sd->state.lr_flag == 2)
				sd->arrow_addsize[type2]+=val;
			break;
		case SP_SUBELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus2: SP_SUBELE: Invalid element %d\n", type2);
				break;
			}
			if(sd->state.lr_flag != 2) {
				if ( type2 == ELE_ALL ) {
					for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ ){
						sd->subele[i] += val;
					}
				} else {
					sd->subele[type2] += val;
				}
			}
			break;
		case SP_SUBRACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_SUBRACE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->subrace[i] += val;
			}
		}
			break;
		case SP_ADDEFF:
			if (type2 > SC_MAX) {
				ShowWarning("pc_bonus2 (Add Effect): %d is not supported.\n", type2);
				break;
			}
			pc->bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
			                 sd->state.lr_flag!=2?val:0, sd->state.lr_flag==2?val:0, 0, 0);
			break;
		case SP_ADDEFF2:
			if (type2 > SC_MAX) {
				ShowWarning("pc_bonus2 (Add Effect2): %d is not supported.\n", type2);
				break;
			}
			pc->bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
			                 sd->state.lr_flag!=2?val:0, sd->state.lr_flag==2?val:0, ATF_SELF, 0);
			break;
		case SP_RESEFF:
			if (type2 < SC_COMMON_MIN || type2 > SC_COMMON_MAX) {
				ShowWarning("pc_bonus2 (Resist Effect): %d is not supported.\n", type2);
				break;
			}
			if(sd->state.lr_flag == 2)
				break;
			i = sd->reseff[type2-SC_COMMON_MIN]+val;
			sd->reseff[type2-SC_COMMON_MIN]= cap_value(i, 0, 10000);
			break;
		case SP_MAGIC_ADDELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus2: SP_MAGIC_ADDELE: Invalid element %d\n", type2);
				break;
			}
			if ( sd->state.lr_flag != 2 ) {
				if ( type2 == ELE_ALL ) {
					for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ )
						sd->magic_addele[i] += val;
				} else {
					sd->magic_addele[type2] += val;
				}
			}
			break;
		case SP_MAGIC_ADDRACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_MAGIC_ADDRACE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->magic_addrace[i] += val;
			}
		}
			break;
		case SP_MAGIC_ADDSIZE:
			if(sd->state.lr_flag != 2)
				sd->magic_addsize[type2]+=val;
			break;
		case SP_MAGIC_ATK_ELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus2: SP_MAGIC_ATK_ELE: Invalid element %d\n", type2);
				break;
			}
			if ( sd->state.lr_flag != 2 ) {
				if ( type2 == ELE_ALL ) {
					for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ )
						sd->magic_atk_ele[i] += val;
				} else {
					sd->magic_atk_ele[type2] += val;
				}
			}
			break;
		case SP_ADD_DAMAGE_CLASS:
			switch (sd->state.lr_flag) {
				case 0: //Right hand
					ARR_FIND(0, ARRAYLENGTH(sd->right_weapon.add_dmg), i, sd->right_weapon.add_dmg[i].rate == 0 || sd->right_weapon.add_dmg[i].class_ == type2);
					if (i == ARRAYLENGTH(sd->right_weapon.add_dmg)) {
						ShowWarning("pc_bonus2: Reached max (%d) number of add Class dmg bonuses per character!\n",
									ARRAYLENGTH(sd->right_weapon.add_dmg));
						break;
					}
					sd->right_weapon.add_dmg[i].class_ = type2;
					sd->right_weapon.add_dmg[i].rate += val;
					if (!sd->right_weapon.add_dmg[i].rate) { //Shift the rest of elements up.
						if( i != ARRAYLENGTH(sd->right_weapon.add_dmg) - 1 )
							memmove(&sd->right_weapon.add_dmg[i], &sd->right_weapon.add_dmg[i+1], sizeof(sd->right_weapon.add_dmg) - (i+1)*sizeof(sd->right_weapon.add_dmg[0]));
					}
					break;
				case 1: //Left hand
					ARR_FIND(0, ARRAYLENGTH(sd->left_weapon.add_dmg), i, sd->left_weapon.add_dmg[i].rate == 0 || sd->left_weapon.add_dmg[i].class_ == type2);
					if (i == ARRAYLENGTH(sd->left_weapon.add_dmg)) {
						ShowWarning("pc_bonus2: Reached max (%d) number of add Class dmg bonuses per character!\n",
									ARRAYLENGTH(sd->left_weapon.add_dmg));
						break;
					}
					sd->left_weapon.add_dmg[i].class_ = type2;
					sd->left_weapon.add_dmg[i].rate += val;
					if (!sd->left_weapon.add_dmg[i].rate) { //Shift the rest of elements up.
						if( i != ARRAYLENGTH(sd->left_weapon.add_dmg) - 1 )
							memmove(&sd->left_weapon.add_dmg[i], &sd->left_weapon.add_dmg[i+1], sizeof(sd->left_weapon.add_dmg) - (i+1)*sizeof(sd->left_weapon.add_dmg[0]));
					}
					break;
			}
			break;
		case SP_ADD_MAGIC_DAMAGE_CLASS:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->add_mdmg), i, sd->add_mdmg[i].rate == 0 || sd->add_mdmg[i].class_ == type2);
			if (i == ARRAYLENGTH(sd->add_mdmg)) {
				ShowWarning("pc_bonus2: Reached max (%d) number of add Class magic dmg bonuses per character!\n", ARRAYLENGTH(sd->add_mdmg));
				break;
			}
			sd->add_mdmg[i].class_ = type2;
			sd->add_mdmg[i].rate += val;
			if (!sd->add_mdmg[i].rate && i != ARRAYLENGTH(sd->add_mdmg) - 1) //Shift the rest of elements up.
				memmove(&sd->add_mdmg[i], &sd->add_mdmg[i+1], sizeof(sd->add_mdmg) - (i+1)*sizeof(sd->add_mdmg[0]));
			break;
		case SP_ADD_DEF_CLASS:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->add_def), i, sd->add_def[i].rate == 0 || sd->add_def[i].class_ == type2);
			if (i == ARRAYLENGTH(sd->add_def)) {
				ShowWarning("pc_bonus2: Reached max (%d) number of add Class def bonuses per character!\n", ARRAYLENGTH(sd->add_def));
				break;
			}
			sd->add_def[i].class_ = type2;
			sd->add_def[i].rate += val;
			if ( !sd->add_def[i].rate && i != ARRAYLENGTH(sd->add_def) - 1) //Shift the rest of elements up.
				memmove(&sd->add_def[i], &sd->add_def[i+1], sizeof(sd->add_def) - (i+1)*sizeof(sd->add_def[0]));
			break;
		case SP_ADD_MDEF_CLASS:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->add_mdef), i, sd->add_mdef[i].rate == 0 || sd->add_mdef[i].class_ == type2);
			if (i == ARRAYLENGTH(sd->add_mdef)) {
				ShowWarning("pc_bonus2: Reached max (%d) number of add Class mdef bonuses per character!\n", ARRAYLENGTH(sd->add_mdef));
				break;
			}
			sd->add_mdef[i].class_ = type2;
			sd->add_mdef[i].rate += val;
			if (!sd->add_mdef[i].rate && i != ARRAYLENGTH(sd->add_mdef) - 1) //Shift the rest of elements up.
				memmove(&sd->add_mdef[i], &sd->add_mdef[i+1], sizeof(sd->add_mdef) - (i+1)*sizeof(sd->add_mdef[0]));
			break;
		case SP_HP_DRAIN_RATE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.hp_drain[RC_NONBOSS].rate += type2;
				sd->right_weapon.hp_drain[RC_NONBOSS].per += val;
				sd->right_weapon.hp_drain[RC_BOSS].rate += type2;
				sd->right_weapon.hp_drain[RC_BOSS].per += val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.hp_drain[RC_NONBOSS].rate += type2;
				sd->left_weapon.hp_drain[RC_NONBOSS].per += val;
				sd->left_weapon.hp_drain[RC_BOSS].rate += type2;
				sd->left_weapon.hp_drain[RC_BOSS].per += val;
			}
			break;
		case SP_HP_DRAIN_VALUE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.hp_drain[RC_NONBOSS].value += type2;
				sd->right_weapon.hp_drain[RC_NONBOSS].type = val;
				sd->right_weapon.hp_drain[RC_BOSS].value += type2;
				sd->right_weapon.hp_drain[RC_BOSS].type = val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.hp_drain[RC_NONBOSS].value += type2;
				sd->left_weapon.hp_drain[RC_NONBOSS].type = val;
				sd->left_weapon.hp_drain[RC_BOSS].value += type2;
				sd->left_weapon.hp_drain[RC_BOSS].type = val;
			}
			break;
		case SP_SP_DRAIN_RATE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.sp_drain[RC_NONBOSS].rate += type2;
				sd->right_weapon.sp_drain[RC_NONBOSS].per += val;
				sd->right_weapon.sp_drain[RC_BOSS].rate += type2;
				sd->right_weapon.sp_drain[RC_BOSS].per += val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.sp_drain[RC_NONBOSS].rate += type2;
				sd->left_weapon.sp_drain[RC_NONBOSS].per += val;
				sd->left_weapon.sp_drain[RC_BOSS].rate += type2;
				sd->left_weapon.sp_drain[RC_BOSS].per += val;
			}
			break;
		case SP_SP_DRAIN_VALUE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.sp_drain[RC_NONBOSS].value += type2;
				sd->right_weapon.sp_drain[RC_NONBOSS].type = val;
				sd->right_weapon.sp_drain[RC_BOSS].value += type2;
				sd->right_weapon.sp_drain[RC_BOSS].type = val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.sp_drain[RC_NONBOSS].value += type2;
				sd->left_weapon.sp_drain[RC_NONBOSS].type = val;
				sd->left_weapon.sp_drain[RC_BOSS].value += type2;
				sd->left_weapon.sp_drain[RC_BOSS].type = val;
			}
			break;
		case SP_HP_VANISH_RATE:
			if (sd->state.lr_flag != 2) {
				sd->bonus.hp_vanish_rate += type2;
				sd->bonus.hp_vanish_per = max(sd->bonus.hp_vanish_per, val);
				sd->bonus.hp_vanish_trigger = 0;
			}
			break;
		case SP_SP_VANISH_RATE:
			if (sd->state.lr_flag != 2) {
				sd->bonus.sp_vanish_rate += type2;
				sd->bonus.sp_vanish_per = max(sd->bonus.sp_vanish_per, val);
				sd->bonus.sp_vanish_trigger = 0;
			}
			break;
		case SP_GET_ZENY_NUM:
			if(sd->state.lr_flag != 2 && sd->bonus.get_zeny_rate < val) {
				sd->bonus.get_zeny_rate = val;
				sd->bonus.get_zeny_num = type2;
			}
			break;
		case SP_ADD_GET_ZENY_NUM:
			if(sd->state.lr_flag != 2) {
				sd->bonus.get_zeny_rate += val;
				sd->bonus.get_zeny_num += type2;
			}
			break;
		case SP_WEAPON_COMA_ELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus2: SP_WEAPON_COMA_ELE: Invalid element %d\n", type2);
				break;
			}
			if(sd->state.lr_flag == 2)
				break;
			if ( type2 == ELE_ALL ) {
				for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ )
					sd->weapon_coma_ele[i] += val;
			} else {
				sd->weapon_coma_ele[type2] += val;
			}
			sd->special_state.bonus_coma = 1;
			break;
		case SP_WEAPON_COMA_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_WEAPON_COMA_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			if(sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->weapon_coma_race[i] += val;
			}
			sd->special_state.bonus_coma = 1;
		}
			break;
		case SP_WEAPON_ATK:
			if(sd->state.lr_flag != 2)
				sd->weapon_atk[type2]+=val;
			break;
		case SP_WEAPON_ATK_RATE:
			if(sd->state.lr_flag != 2)
				sd->weapon_atk_rate[type2]+=val;
			break;
		case SP_CRITICAL_ADDRACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_CRITICAL_ADDRACE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->critaddrace[i] += val*10;
			}
		}
			break;
		case SP_ADDEFF_WHENHIT:
			if (type2 > SC_MAX) {
				ShowWarning("pc_bonus2 (Add Effect when hit): %d is not supported.\n", type2);
				break;
			}
			if(sd->state.lr_flag != 2)
				pc->bonus_addeff(sd->addeff2, ARRAYLENGTH(sd->addeff2), (sc_type)type2, val, 0, 0, 0);
			break;
		case SP_SKILL_ATK:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillatk), i, sd->skillatk[i].id == 0 || sd->skillatk[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillatk)) {
				//Better mention this so the array length can be updated. [Skotlex]
				ShowDebug("script->run: bonus2 bSkillAtk reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillatk), type2, val);
				break;
			}
			if (sd->skillatk[i].id == type2)
				sd->skillatk[i].val += val;
			else {
				sd->skillatk[i].id = type2;
				sd->skillatk[i].val = val;
			}
			break;
		case SP_SKILL_HEAL:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillheal), i, sd->skillheal[i].id == 0 || sd->skillheal[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillheal)) {
				// Better mention this so the array length can be updated. [Skotlex]
				ShowDebug("script->run: bonus2 bSkillHeal reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillheal), type2, val);
				break;
			}
			if (sd->skillheal[i].id == type2)
				sd->skillheal[i].val += val;
			else {
				sd->skillheal[i].id = type2;
				sd->skillheal[i].val = val;
			}
			break;
		case SP_SKILL_HEAL2:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillheal2), i, sd->skillheal2[i].id == 0 || sd->skillheal2[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillheal2)) {
				// Better mention this so the array length can be updated. [Skotlex]
				ShowDebug("script->run: bonus2 bSkillHeal2 reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillheal2), type2, val);
				break;
			}
			if (sd->skillheal2[i].id == type2)
				sd->skillheal2[i].val += val;
			else {
				sd->skillheal2[i].id = type2;
				sd->skillheal2[i].val = val;
			}
			break;
		case SP_ADD_SKILL_BLOW:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillblown), i, sd->skillblown[i].id == 0 || sd->skillblown[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillblown)) {
				//Better mention this so the array length can be updated. [Skotlex]
				ShowDebug("script->run: bonus2 bSkillBlown reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillblown), type2, val);
				break;
			}
			if(sd->skillblown[i].id == type2)
				sd->skillblown[i].val += val;
			else {
				sd->skillblown[i].id = type2;
				sd->skillblown[i].val = val;
			}
			break;
#ifndef RENEWAL_CAST
		case SP_VARCASTRATE:
#endif
		case SP_CASTRATE:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillcast), i, sd->skillcast[i].id == 0 || sd->skillcast[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillcast)) {
				//Better mention this so the array length can be updated. [Skotlex]
				ShowDebug("script->run: bonus2 %s reached its limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
					type == SP_CASTRATE ? "bCastRate" : "bVariableCastrate",
					ARRAYLENGTH(sd->skillcast), type2, val);
				break;
			}
			if(sd->skillcast[i].id == type2)
				sd->skillcast[i].val += val;
			else {
				sd->skillcast[i].id = type2;
				sd->skillcast[i].val = val;
			}
			break;

		case SP_FIXCASTRATE:
			if(sd->state.lr_flag == 2)
				break;

			ARR_FIND(0, ARRAYLENGTH(sd->skillfixcastrate), i, sd->skillfixcastrate[i].id == 0 || sd->skillfixcastrate[i].id == type2);

			if (i == ARRAYLENGTH(sd->skillfixcastrate)) {
				ShowDebug("script->run: bonus2 bFixedCastrate reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillfixcastrate), type2, val);
				break;
			}

			if(sd->skillfixcastrate[i].id == type2)
				sd->skillfixcastrate[i].val -= val;

			else {
				sd->skillfixcastrate[i].id = type2;
				sd->skillfixcastrate[i].val -= val;
			}

			break;

		case SP_HP_LOSS_RATE:
			if(sd->state.lr_flag != 2) {
				sd->hp_loss.value = type2;
				sd->hp_loss.rate = val;
			}
			break;
		case SP_HP_REGEN_RATE:
			if(sd->state.lr_flag != 2) {
				sd->hp_regen.value = type2;
				sd->hp_regen.rate = val;
			}
			break;
		case SP_ADDRACE2:
			if (!(type2 > RC2_NONE && type2 < RC2_MAX))
				break;
			if(sd->state.lr_flag != 2)
				sd->right_weapon.addrace2[type2] += val;
			else
				sd->left_weapon.addrace2[type2] += val;
			break;
		case SP_SUBSIZE:
			if(sd->state.lr_flag != 2)
				sd->subsize[type2]+=val;
			break;
		case SP_SUBRACE2:
			if (!(type2 > RC2_NONE && type2 < RC2_MAX))
				break;
			if(sd->state.lr_flag != 2)
				sd->subrace2[type2]+=val;
			break;
		case SP_ADD_ITEM_HEAL_RATE:
			if(sd->state.lr_flag == 2)
				break;
			//Standard item bonus.
			for(i=0; i < ARRAYLENGTH(sd->itemhealrate) && sd->itemhealrate[i].nameid && sd->itemhealrate[i].nameid != type2; i++);
			if (i == ARRAYLENGTH(sd->itemhealrate)) {
				ShowWarning("pc_bonus2: Reached max (%d) number of item heal bonuses per character!\n", ARRAYLENGTH(sd->itemhealrate));
				break;
			}
			sd->itemhealrate[i].nameid = type2;
			sd->itemhealrate[i].rate += val;
			break;
		case SP_EXP_ADDRACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_EXP_ADDRACE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->expaddrace[i] += val;
			}
		}
			break;
		case SP_SP_GAIN_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_SP_GAIN_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->sp_gain_race[i] += val;
			}
		}
			break;
		case SP_ADD_MONSTER_DROP_ITEM:
			if (sd->state.lr_flag != 2)
				pc->bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), type2, false, map->race_id2mask(RC_ALL), val);
			break;
		case SP_SP_LOSS_RATE:
			if(sd->state.lr_flag != 2) {
				sd->sp_loss.value = type2;
				sd->sp_loss.rate = val;
			}
			break;
		case SP_SP_REGEN_RATE:
			if(sd->state.lr_flag != 2) {
				sd->sp_regen.value = type2;
				sd->sp_regen.rate = val;
			}
			break;
		case SP_HP_DRAIN_VALUE_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_HP_DRAIN_VALUE_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				if (sd->state.lr_flag == 0)
					sd->right_weapon.hp_drain[i].value += val;
				else if(sd->state.lr_flag == 1)
					sd->left_weapon.hp_drain[i].value += val;
			}
		}
			break;
		case SP_SP_DRAIN_VALUE_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_SP_DRAIN_VALUE_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				if (sd->state.lr_flag == 0)
					sd->right_weapon.sp_drain[i].value += val;
				else if (sd->state.lr_flag == 1)
					sd->left_weapon.sp_drain[i].value += val;
			}
		}
			break;
		case SP_IGNORE_MDEF_RATE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_IGNORE_MDEF_RATE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->ignore_mdef[i] += val;
			}
		}
			break;
		case SP_IGNORE_DEF_RATE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_IGNORE_DEF_RATE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->ignore_def[i] += val;
			}
		}
			break;
		case SP_SP_GAIN_RACE_ATTACK:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_SP_GAIN_RACE_ATTACK: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->sp_gain_race_attack[i] = cap_value(sd->sp_gain_race_attack[i] + val, 0, INT16_MAX);
			}
		}
			break;
		case SP_HP_GAIN_RACE_ATTACK:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_HP_GAIN_RACE_ATTACK: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->hp_gain_race_attack[i] = cap_value(sd->hp_gain_race_attack[i] + val, 0, INT16_MAX);
			}
		}
			break;
		case SP_SKILL_USE_SP_RATE: //bonus2 bSkillUseSPrate,n,x;
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillusesprate), i, sd->skillusesprate[i].id == 0 || sd->skillusesprate[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillusesprate)) {
				ShowDebug("script->run: bonus2 bSkillUseSPrate reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillusesprate), type2, val);
				break;
			}
			if (sd->skillusesprate[i].id == type2)
				sd->skillusesprate[i].val += val;
			else {
				sd->skillusesprate[i].id = type2;
				sd->skillusesprate[i].val = val;
			}
			break;
		case SP_SKILL_COOLDOWN:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillcooldown), i, sd->skillcooldown[i].id == 0 || sd->skillcooldown[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillcooldown)) {
				ShowDebug("script->run: bonus2 bSkillCoolDown reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillcooldown), type2, val);
				break;
			}
			if (sd->skillcooldown[i].id == type2)
				sd->skillcooldown[i].val += val;
			else {
				sd->skillcooldown[i].id = type2;
				sd->skillcooldown[i].val = val;
			}
			break;
		case SP_SKILL_FIXEDCAST:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillfixcast), i, sd->skillfixcast[i].id == 0 || sd->skillfixcast[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillfixcast)) {
				ShowDebug("script->run: bonus2 bSkillFixedCast reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillfixcast), type2, val);
				break;
			}
			if (sd->skillfixcast[i].id == type2)
				sd->skillfixcast[i].val += val;
			else {
				sd->skillfixcast[i].id = type2;
				sd->skillfixcast[i].val = val;
			}
			break;
		case SP_SKILL_VARIABLECAST:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillvarcast), i, sd->skillvarcast[i].id == 0 || sd->skillvarcast[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillvarcast)) {
				ShowDebug("script->run: bonus2 bSkillVariableCast reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillvarcast), type2, val);
				break;
			}
			if (sd->skillvarcast[i].id == type2)
				sd->skillvarcast[i].val += val;
			else {
				sd->skillvarcast[i].id = type2;
				sd->skillvarcast[i].val = val;
			}
			break;
	#ifdef RENEWAL_CAST
		case SP_VARCASTRATE:
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillcast), i, sd->skillcast[i].id == 0 || sd->skillcast[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillcast)) {
				ShowDebug("script->run: bonus2 bVariableCastrate reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillcast), type2, val);
				break;
			}
			if(sd->skillcast[i].id == type2)
				sd->skillcast[i].val -= val;
			else {
				sd->skillcast[i].id = type2;
				sd->skillcast[i].val -= val;
			}
			break;
	#endif
		case SP_SKILL_USE_SP: //bonus2 bSkillUseSP,n,x;
			if(sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->skillusesp), i, sd->skillusesp[i].id == 0 || sd->skillusesp[i].id == type2);
			if (i == ARRAYLENGTH(sd->skillusesp)) {
				ShowDebug("script->run: bonus2 bSkillUseSP reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->skillusesp), type2, val);
				break;
			}
			if (sd->skillusesp[i].id == type2)
				sd->skillusesp[i].val += val;
			else {
				sd->skillusesp[i].id = type2;
				sd->skillusesp[i].val = val;
			}
			break;
		case SP_ADD_MONSTER_DROP_CHAINITEM:
		{
			uint32 race_mask = map->race_id2mask(val);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_ADD_MONSTER_DROP_CHAINITEM: Invalid Race (%d)\n", val);
				break;
			}
			if (sd->state.lr_flag != 2)
				pc->bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), type2, true, race_mask, 10000);
		}
			break;
#ifdef RENEWAL
		case SP_RACE_TOLERANCE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_RACE_TOLERANCE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask)
				sd->race_tolerance[i] += val;
			}
			break;
#endif
		case SP_SUB_SKILL:
			if (sd->state.lr_flag == 2)
				break;
			ARR_FIND(0, ARRAYLENGTH(sd->subskill), i, sd->subskill[i].id == 0 || sd->subskill[i].id == type2);
			if (i == ARRAYLENGTH(sd->subskill)) {
				ShowDebug("script->run: bonus2 bSubSkill reached it's limit (%d skills per character), bonus skill %d (+%d%%) lost.\n",
				          ARRAYLENGTH(sd->subskill), type2, val);
				break;
			}
			if (sd->subskill[i].id == type2) {
				sd->subskill[i].val += val;
			} else {
				sd->subskill[i].id = type2;
				sd->subskill[i].val = val;
			}
			break;
		case SP_ADD_DROP_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_ADD_DROP_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			if (sd->state.lr_flag == 2)
				break;
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask)
				sd->dropaddrace[i] += val;
		}
			break;
		default:
			ShowWarning("pc_bonus2: unknown type %d %d %d!\n",type,type2,val);
			Assert_report(0);
			break;
	}
	return 0;
}

static int pc_bonus3(struct map_session_data *sd, int type, int type2, int type3, int val)
{
	int i;
	nullpo_ret(sd);

	switch(type){
		case SP_ADD_MONSTER_DROP_ITEM:
		{
			uint32 race_mask = map->race_id2mask(type3);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus2: SP_ADD_MONSTER_DROP_ITEM: Invalid Race (%d)\n", type3);
				break;
			}
			if (sd->state.lr_flag != 2)
				pc->bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), type2, false, race_mask, val);
		}
			break;
		case SP_ADD_CLASS_DROP_ITEM:
			if(sd->state.lr_flag != 2)
				pc->bonus_item_drop(sd->add_drop, ARRAYLENGTH(sd->add_drop), type2, false, -type3, val);
			break;
		case SP_AUTOSPELL:
			if(sd->state.lr_flag != 2)
			{
				int target = skill->get_inf(type2); //Support or Self (non-auto-target) skills should pick self.
				target = target&INF_SUPPORT_SKILL || (target&INF_SELF_SKILL && !(skill->get_inf2(type2)&INF2_NO_TARGET_SELF));
				pc->bonus_autospell(sd->autospell, ARRAYLENGTH(sd->autospell),
					target?-type2:type2, type3, val, 0, status->current_equip_card_id);
			}
			break;
		case SP_AUTOSPELL_WHENHIT:
			if(sd->state.lr_flag != 2)
			{
				int target = skill->get_inf(type2); //Support or Self (non-auto-target) skills should pick self.
				target = target&INF_SUPPORT_SKILL || (target&INF_SELF_SKILL && !(skill->get_inf2(type2)&INF2_NO_TARGET_SELF));
				pc->bonus_autospell(sd->autospell2, ARRAYLENGTH(sd->autospell2),
					target?-type2:type2, type3, val, BF_NORMAL|BF_SKILL, status->current_equip_card_id);
			}
			break;
		case SP_SP_DRAIN_RATE:
			if (sd->state.lr_flag == 0) {
				// Decomposed RC_ALL:
				sd->right_weapon.sp_drain[RC_NONBOSS].rate += type2;
				sd->right_weapon.sp_drain[RC_NONBOSS].per += type3;
				sd->right_weapon.sp_drain[RC_NONBOSS].type = val;
				sd->right_weapon.sp_drain[RC_BOSS].rate += type2;
				sd->right_weapon.sp_drain[RC_BOSS].per += type3;
				sd->right_weapon.sp_drain[RC_BOSS].type = val;
			} else if (sd->state.lr_flag == 1) {
				// Decomposed RC_ALL:
				sd->left_weapon.sp_drain[RC_NONBOSS].rate += type2;
				sd->left_weapon.sp_drain[RC_NONBOSS].per += type3;
				sd->left_weapon.sp_drain[RC_NONBOSS].type = val;
				sd->left_weapon.sp_drain[RC_BOSS].rate += type2;
				sd->left_weapon.sp_drain[RC_BOSS].per += type3;
				sd->left_weapon.sp_drain[RC_BOSS].type = val;
			}
			break;
		case SP_HP_DRAIN_RATE_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus3: SP_HP_DRAIN_RATE_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				if (sd->state.lr_flag == 0) {
					sd->right_weapon.hp_drain[i].rate += type3;
					sd->right_weapon.hp_drain[i].per += val;
				} else if(sd->state.lr_flag == 1) {
					sd->left_weapon.hp_drain[i].rate += type3;
					sd->left_weapon.hp_drain[i].per += val;
				}
			}
		}
			break;
		case SP_SP_DRAIN_RATE_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus3: SP_SP_DRAIN_RATE_RACE: Invalid Race (%d)\n", type2);
				break;
			}
			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				if (sd->state.lr_flag == 0) {
					sd->right_weapon.sp_drain[i].rate += type3;
					sd->right_weapon.sp_drain[i].per += val;
				} else if(sd->state.lr_flag == 1) {
					sd->left_weapon.sp_drain[i].rate += type3;
					sd->left_weapon.sp_drain[i].per += val;
				}
			}
		}
			break;
		case SP_ADDEFF:
			if (type2 > SC_MAX) {
				ShowWarning("pc_bonus3 (Add Effect): %d is not supported.\n", type2);
				break;
			}
			pc->bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
			                 sd->state.lr_flag!=2?type3:0, sd->state.lr_flag==2?type3:0, val, 0);
			break;

		case SP_ADDEFF_WHENHIT:
			if (type2 > SC_MAX) {
				ShowWarning("pc_bonus3 (Add Effect when hit): %d is not supported.\n", type2);
				break;
			}
			if(sd->state.lr_flag != 2)
				pc->bonus_addeff(sd->addeff2, ARRAYLENGTH(sd->addeff2), (sc_type)type2, type3, 0, val, 0);
			break;

		case SP_ADDEFF_ONSKILL:
			if( type3 > SC_MAX ) {
				ShowWarning("pc_bonus3 (Add Effect on skill): %d is not supported.\n", type3);
				break;
			}
			if( sd->state.lr_flag != 2 )
				pc->bonus_addeff_onskill(sd->addeff3, ARRAYLENGTH(sd->addeff3), (sc_type)type3, val, type2, ATF_TARGET);
			break;

		case SP_ADDELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus3: SP_ADDELE: Invalid element %d\n", type2);
				break;
			}
			if ( sd->state.lr_flag != 2 ) {
				if ( type2 == ELE_ALL ) {
					for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ )
						pc->bonus_addele(sd, (unsigned char)i, type3, val);
				} else {
					pc->bonus_addele(sd, (unsigned char)type2, type3, val);
				}
			}
			break;

		case SP_SUBELE:
			if( (type2 >= ELE_MAX && type2 != ELE_ALL) || (type2 < ELE_NEUTRAL) ) {
				ShowError("pc_bonus3: SP_SUBELE: Invalid element %d\n", type2);
				break;
			}
			if ( sd->state.lr_flag != 2 ) {
				if ( type2 == ELE_ALL ) {
					for ( i = ELE_NEUTRAL; i < ELE_MAX; i++ )
						pc->bonus_subele(sd, (unsigned char)i, type3, val);
				} else {
					pc->bonus_subele(sd, (unsigned char)type2, type3, val);
				}
			}
			break;
		case SP_HP_VANISH_RATE:
			if (sd->state.lr_flag != 2) {
				sd->bonus.hp_vanish_rate += type2;
				sd->bonus.hp_vanish_per = max(sd->bonus.hp_vanish_per, type3);
				sd->bonus.hp_vanish_trigger = val;
			}
			break;
		case SP_SP_VANISH_RATE:
			if (sd->state.lr_flag != 2) {
				sd->bonus.sp_vanish_rate += type2;
				sd->bonus.sp_vanish_per = max(sd->bonus.sp_vanish_per, type3);
				sd->bonus.sp_vanish_trigger = val;
			}
			break;
		case SP_SUB_DEF_ELE:
			if ((type2 >= ELE_MAX && type2 != ELE_ALL) || type2 < ELE_NEUTRAL) {
				ShowError("pc_bonus3: SP_SUB_DEF_ELE: Invalid element %d\n", type2);
				break;
			}

			if (type2 == ELE_ALL) {
				for (int j = ELE_NEUTRAL; j < ELE_MAX; j++) {
					if ((val & 1) != 0)
						sd->sub_def_ele[j].rate_mob += type3;

					if ((val & 2) != 0)
						sd->sub_def_ele[j].rate_pc += type3;
				}
			} else {
				if ((val & 1) != 0)
					sd->sub_def_ele[type2].rate_mob += type3;

				if ((val & 2) != 0)
					sd->sub_def_ele[type2].rate_pc += type3;
			}

			break;
		case SP_MAGIC_SUB_DEF_ELE:
			if ((type2 >= ELE_MAX && type2 != ELE_ALL) || type2 < ELE_NEUTRAL) {
				ShowError("pc_bonus3: SP_MAGIC_SUB_DEF_ELE: Invalid element %d\n", type2);
				break;
			}

			if (type2 == ELE_ALL) {
				for (int j = ELE_NEUTRAL; j < ELE_MAX; j++) {
					if ((val & 1) != 0)
						sd->magic_sub_def_ele[j].rate_mob += type3;

					if ((val & 2) != 0)
						sd->magic_sub_def_ele[j].rate_pc += type3;
				}
			} else {
				if ((val & 1) != 0)
					sd->magic_sub_def_ele[type2].rate_mob += type3;

				if ((val & 2) != 0)
					sd->magic_sub_def_ele[type2].rate_pc += type3;
			}

			break;
		case SP_STATE_NO_RECOVER_RACE:
		{
			uint32 race_mask = map->race_id2mask(type2);
			if (race_mask == RCMASK_NONE) {
				ShowWarning("pc_bonus3: SP_STATE_NO_RECOVER_RACE: Invalid Race (%d)\n", type2);
				break;
			}

			if (sd->state.lr_flag == 2)
				break;

			BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
				sd->no_recover_state_race[i].rate = type3;
				sd->no_recover_state_race[i].tick = val;
			}
		}
			break;
		default:
			ShowWarning("pc_bonus3: unknown type %d %d %d %d!\n",type,type2,type3,val);
			Assert_report(0);
			break;
	}

	return 0;
}

static int pc_bonus4(struct map_session_data *sd, int type, int type2, int type3, int type4, int val)
{
	int i;
	nullpo_ret(sd);

	switch(type) {
	case SP_AUTOSPELL:
		if(sd->state.lr_flag != 2)
			pc->bonus_autospell(sd->autospell, ARRAYLENGTH(sd->autospell), (val&1) ? type2 : -type2, (val&2) ? -type3 : type3, type4, 0, status->current_equip_card_id);
		break;

	case SP_AUTOSPELL_WHENHIT:
		if(sd->state.lr_flag != 2)
			pc->bonus_autospell(sd->autospell2, ARRAYLENGTH(sd->autospell2), (val&1) ? type2 : -type2, (val&2) ? -type3 : type3, type4, BF_NORMAL|BF_SKILL, status->current_equip_card_id);
		break;

	case SP_AUTOSPELL_ONSKILL:
		if(sd->state.lr_flag != 2) {
			int target = skill->get_inf(type2); //Support or Self (non-auto-target) skills should pick self.
			target = target&INF_SUPPORT_SKILL || (target&INF_SELF_SKILL && !(skill->get_inf2(type2)&INF2_NO_TARGET_SELF));

			pc->bonus_autospell_onskill(sd->autospell3, ARRAYLENGTH(sd->autospell3), type2, target?-type3:type3, type4, val, status->current_equip_card_id);
		}
		break;

	case SP_ADDEFF_ONSKILL:
		if( type2 > SC_MAX ) {
			ShowWarning("pc_bonus4 (Add Effect on skill): %d is not supported.\n", type2);
			break;
		}
		if( sd->state.lr_flag != 2 )
			pc->bonus_addeff_onskill(sd->addeff3, ARRAYLENGTH(sd->addeff3), (sc_type)type3, type4, type2, val);
		break;

	case SP_SET_DEF_RACE: //bonus4 bSetDefRace,n,x,r,y;
	{
		uint32 race_mask = map->race_id2mask(type2);
		if (race_mask == RCMASK_NONE) {
			ShowWarning("pc_bonus4: SP_SET_DEF_RACE: Invalid Race (%d)\n", type2);
			break;
		}
		if (sd->state.lr_flag == 2)
			break;
		BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
			sd->def_set_race[i].rate = type3;
			sd->def_set_race[i].tick = type4;
			sd->def_set_race[i].value = val;
		}
	}
		break;

	case SP_SET_MDEF_RACE: //bonus4 bSetMDefRace,n,x,r,y;
	{
		uint32 race_mask = map->race_id2mask(type2);
		if (race_mask == RCMASK_NONE) {
			ShowWarning("pc_bonus4: SP_SET_MDEF_RACE: Invalid Race (%d)\n", type2);
			break;
		}
		if (sd->state.lr_flag == 2)
			break;
		BONUS_FOREACH_RCARRAY_FROMMASK(i, race_mask) {
			sd->mdef_set_race[i].rate = type3;
			sd->mdef_set_race[i].tick = type4;
			sd->mdef_set_race[i].value = val;
		}
	}
		break;

	case SP_ADDEFF:
	{
		uint16 duration;
		if (type2 > SC_MAX) {
			ShowWarning("pc_bonus4 (Add Effect): %d is not supported.\n", type2);
			break;
		}
		if (val < 0 || val > UINT16_MAX) {
			ShowWarning("pc_bonus4 (Add Effect): invalid duration %d. Valid range: [0:%d].\n", val, UINT16_MAX);
			duration = (val < 0 ? 0 : UINT16_MAX);
		} else {
			duration = (uint16)val;
		}

		pc->bonus_addeff(sd->addeff, ARRAYLENGTH(sd->addeff), (sc_type)type2,
		                 sd->state.lr_flag!=2?type3:0, sd->state.lr_flag==2?type3:0, type4, duration);
	}
		break;

	default:
		ShowWarning("pc_bonus4: unknown type %d %d %d %d %d!\n",type,type2,type3,type4,val);
		Assert_report(0);
		break;
	}

	return 0;
}

static int pc_bonus5(struct map_session_data *sd, int type, int type2, int type3, int type4, int type5, int val)
{
	nullpo_ret(sd);

	switch(type){
		case SP_AUTOSPELL:
			if(sd->state.lr_flag != 2)
				pc->bonus_autospell(sd->autospell, ARRAYLENGTH(sd->autospell), (val&1) ? type2 : -type2, (val&2) ? -type3 : type3, type4, type5, status->current_equip_card_id);
			break;

		case SP_AUTOSPELL_WHENHIT:
			if(sd->state.lr_flag != 2)
				pc->bonus_autospell(sd->autospell2, ARRAYLENGTH(sd->autospell2), (val&1) ? type2 : -type2, (val&2) ? -type3 : type3, type4, type5, status->current_equip_card_id);
			break;

		case SP_AUTOSPELL_ONSKILL:
			if(sd->state.lr_flag != 2)
				pc->bonus_autospell_onskill(sd->autospell3, ARRAYLENGTH(sd->autospell3), type2, (val&1) ? -type3 : type3, (val&2) ? -type4 : type4, type5, status->current_equip_card_id);
			break;

		default:
			ShowWarning("pc_bonus5: unknown type %d %d %d %d %d %d!\n",type,type2,type3,type4,type5,val);
			Assert_report(0);
			break;
	}

	return 0;
}

#undef BONUS_FOREACH_RCARRAY_FROMMASK

/*==========================================
 * Grants a player a given skill.
 * Flag values: @see enum pc_skill_flag
 *------------------------------------------*/
static int pc_skill(struct map_session_data *sd, int id, int level, int flag)
{
	int index = 0;
	nullpo_ret(sd);

	if (!(index = skill->get_index(id))) {
		ShowError("pc_skill: Skill with id %d does not exist in the skill database\n", id);
		return 0;
	}
	if( level > MAX_SKILL_LEVEL ) {
		ShowError("pc_skill: Skill level %d too high. Max lv supported is %d\n", level, MAX_SKILL_LEVEL);
		return 0;
	}
	if( flag == SKILL_GRANT_TEMPSTACK && sd->status.skill[index].lv + level > MAX_SKILL_LEVEL ) {
		ShowError("pc_skill: Skill level bonus %d too high. Max lv supported is %d. Curr lv is %d\n", level, MAX_SKILL_LEVEL, sd->status.skill[index].lv);
		return 0;
	}

	switch( flag ){
		case SKILL_GRANT_PERMANENT: //Set skill data overwriting whatever was there before.
			sd->status.skill[index].id   = id;
			sd->status.skill[index].lv   = level;
			sd->status.skill[index].flag = SKILL_FLAG_PERMANENT;
			if( level == 0 ) { //Remove skill.
				sd->status.skill[index].id = 0;
				clif->deleteskill(sd,id);
			} else
				clif->addskill(sd,id);
			if( !skill->dbs->db[index].inf ) //Only recalculate for passive skills.
				status_calc_pc(sd, SCO_NONE);
		break;
		case SKILL_GRANT_TEMPORARY: //Item bonus skill.
			if( sd->status.skill[index].id == id ) {
				if( sd->status.skill[index].lv >= level )
					return 0;
				if (sd->status.skill[index].flag == SKILL_FLAG_PERMANENT) // Non-granted skill, store its level.
					sd->status.skill[index].flag = SKILL_FLAG_REPLACED_LV_0 + sd->status.skill[index].lv;
			} else {
				sd->status.skill[index].id   = id;
				sd->status.skill[index].flag = SKILL_FLAG_TEMPORARY;
			}
			sd->status.skill[index].lv = level;
		break;
		case SKILL_GRANT_TEMPSTACK: //Add skill bonus on top of what you had.
			if( sd->status.skill[index].id == id ) {
				if( sd->status.skill[index].flag == SKILL_FLAG_PERMANENT )
					sd->status.skill[index].flag = SKILL_FLAG_REPLACED_LV_0 + sd->status.skill[index].lv; // Store previous level.
			} else {
				sd->status.skill[index].id   = id;
				sd->status.skill[index].flag = SKILL_FLAG_TEMPORARY; //Set that this is a bonus skill.
			}
			sd->status.skill[index].lv += level;
		break;
		case SKILL_GRANT_UNCONDITIONAL:
			sd->status.skill[index].id   = id;
			sd->status.skill[index].lv   = level;
			sd->status.skill[index].flag = SKILL_FLAG_PERM_GRANTED;
			if( level == 0 ) { //Remove skill.
				sd->status.skill[index].id = 0;
				clif->deleteskill(sd,id);
			} else
				clif->addskill(sd,id);
			if( !skill->dbs->db[index].inf ) //Only recalculate for passive skills.
				status_calc_pc(sd, SCO_NONE);
			break;
		default: //Unknown flag?
			return 0;
	}
	return 1;
}

/**
 * Checks if the given card can be inserted into the given equipment piece.
 *
 * @param sd        The current character.
 * @param idx_card  The card's inventory index (note: it must be a valid index and can be checked by pc_can_insert_card)
 * @param idx_equip The target equipment's inventory index.
 * @retval true if the card can be inserted.
 */
static bool pc_can_insert_card_into(struct map_session_data *sd, int idx_card, int idx_equip)
{
	int i;

	nullpo_ret(sd);

	if (idx_equip < 0 || idx_equip >= sd->status.inventorySize || sd->inventory_data[idx_equip] == NULL)
		return false; //Invalid item index.
	if (sd->status.inventory[idx_equip].nameid <= 0 || sd->status.inventory[idx_equip].amount < 1)
		return false; // target item missing
	if (sd->inventory_data[idx_equip]->type != IT_WEAPON && sd->inventory_data[idx_equip]->type != IT_ARMOR)
		return false; // only weapons and armor are allowed
	if (sd->status.inventory[idx_equip].identify == 0)
		return false; // target must be identified
	if (itemdb_isspecial(sd->status.inventory[idx_equip].card[0]))
		return false; // card slots reserved for other purposes
	if (sd->status.inventory[idx_equip].equip != 0)
		return false; // item must be unequipped
	if ((sd->inventory_data[idx_equip]->equip & sd->inventory_data[idx_card]->equip) == 0)
		return false; // card cannot be compounded on this item type
	if (sd->inventory_data[idx_equip]->type == IT_WEAPON && sd->inventory_data[idx_card]->equip == EQP_SHIELD)
		return false; // attempted to place shield card on left-hand weapon.

	ARR_FIND( 0, sd->inventory_data[idx_equip]->slot, i, sd->status.inventory[idx_equip].card[i] == 0);
	if (i == sd->inventory_data[idx_equip]->slot)
		return false; // no free slots
	return true;
}

/**
 * Checks if the given item is card and it can be inserted into some equipment.
 *
 * @param sd        The current character.
 * @param idx_card  The card's inventory index.
 * @retval true if the card can be inserted.
 */
static bool pc_can_insert_card(struct map_session_data *sd, int idx_card)
{
	nullpo_ret(sd);

	if (idx_card < 0 || idx_card >= sd->status.inventorySize || sd->inventory_data[idx_card] == NULL)
		return false; //Invalid card index.
	if (sd->status.inventory[idx_card].nameid <= 0 || sd->status.inventory[idx_card].amount < 1)
		return false; // target card missing
	if (sd->inventory_data[idx_card]->type != IT_CARD)
		return false; // must be a card
	return true;
}

/*==========================================
 * Attempt to insert card into item.
 * Return:
 *   0 = fail
 *   1 = success
 *------------------------------------------*/
static int pc_insert_card(struct map_session_data *sd, int idx_card, int idx_equip)
{
	int nameid;

	nullpo_ret(sd);

	if (sd->state.trading != 0)
		return 0;

	if (!pc->can_insert_card(sd, idx_card) || !pc->can_insert_card_into(sd, idx_card, idx_equip))
		return 0;

	// remember the card id to insert
	nameid = sd->status.inventory[idx_card].nameid;

	if( pc->delitem(sd, idx_card, 1, 1, DELITEM_NORMAL, LOG_TYPE_CARD) == 1 )
	{// failed
		clif->insert_card(sd,idx_equip,idx_card,1);
	}
	else
	{// success
		int i;
		ARR_FIND( 0, sd->inventory_data[idx_equip]->slot, i, sd->status.inventory[idx_equip].card[i] == 0);
		if (i == sd->inventory_data[idx_equip]->slot)
			return 0; // no free slots
		logs->pick_pc(sd, LOG_TYPE_CARD, -1, &sd->status.inventory[idx_equip],sd->inventory_data[idx_equip]);
		sd->status.inventory[idx_equip].card[i] = nameid;
		logs->pick_pc(sd, LOG_TYPE_CARD,  1, &sd->status.inventory[idx_equip],sd->inventory_data[idx_equip]);
		clif->insert_card(sd,idx_equip,idx_card,0);
		return 1;
	}

	return 0;
}

//
// Items
//

/*==========================================
 * Update buying value by skills
 *------------------------------------------*/
static int pc_modifybuyvalue(struct map_session_data *sd, int orig_value, bool ignore_discount)
{
	if (!ignore_discount) {
		int skill_lv, rate1 = 0, rate2 = 0;

		if ((skill_lv = pc->checkskill(sd, MC_DISCOUNT)) > 0) // Merchant Discount
			rate1 = 5 + skill_lv * 2 - ((skill_lv == 10) ? 1 : 0);
		if ((skill_lv = pc->checkskill(sd, RG_COMPULSION)) > 0) // Rogue Discount
			rate2 = 5 + skill_lv * 4;
		if (rate1 < rate2)
			rate1 = rate2;
		if (rate1 != 0)
			orig_value = apply_percentrate(orig_value, 100 - rate1, 100);
	}

	if (orig_value < battle_config.min_item_buy_price)
		orig_value = battle_config.min_item_buy_price;

	return orig_value;
}

/*==========================================
 * Update selling value by skills
 *------------------------------------------*/
static int pc_modifysellvalue(struct map_session_data *sd, int orig_value, bool ignore_overcharge)
{
	if (!ignore_overcharge) {
		int skill_lv, rate = 0;

		if ((skill_lv = pc->checkskill(sd, MC_OVERCHARGE)) > 0) // Merchant Overcharge
			rate = 5 + skill_lv * 2 - ((skill_lv == 10) ? 1 : 0);
		if (rate != 0)
			orig_value = apply_percentrate(orig_value, 100 + rate, 100);
	}

	if (orig_value < battle_config.min_item_sell_price)
		orig_value = battle_config.min_item_sell_price;

	return orig_value;
}

/*==========================================
 * Checking if we have enough place on inventory for new item
 * Make sure to take 30k as limit (for client I guess)
 *------------------------------------------*/
static int pc_checkadditem(struct map_session_data *sd, int nameid, int amount)
{
	struct item_data* data;

	nullpo_ret(sd);

	if(amount > MAX_AMOUNT)
		return ADDITEM_OVERAMOUNT;

	data = itemdb->search(nameid);

	if(!itemdb->isstackable2(data))
		return ADDITEM_NEW;

	if( data->stack.inventory && amount > data->stack.amount )
		return ADDITEM_OVERAMOUNT;

	for(int i = 0; i < sd->status.inventorySize; i++) {
		// FIXME: This does not consider the checked item's cards, thus could check a wrong slot for stackability.
		if(sd->status.inventory[i].nameid==nameid){
			if( amount > MAX_AMOUNT - sd->status.inventory[i].amount || ( data->stack.inventory && amount > data->stack.amount - sd->status.inventory[i].amount ) )
				return ADDITEM_OVERAMOUNT;
			return ADDITEM_EXIST;
		}
	}

	return ADDITEM_NEW;
}

/*==========================================
 * Return number of available place in inventory
 * Each non stackable item will reduce place by 1
 *------------------------------------------*/
static int pc_inventoryblank(struct map_session_data *sd)
{
	nullpo_ret(sd);
	int b = 0;

	for (int i = 0; i < sd->status.inventorySize; i++) {
		if(sd->status.inventory[i].nameid==0)
			b++;
	}

	return b;
}

/*==========================================
 * attempts to remove zeny from player (sd)
 *------------------------------------------*/
static int pc_payzeny(struct map_session_data *sd, int zeny, enum e_log_pick_type type, struct map_session_data *tsd)
{
	nullpo_retr(-1,sd);

	zeny = cap_value(zeny,-MAX_ZENY,MAX_ZENY); //prevent command UB
	if( zeny < 0 )
	{
		ShowError("pc_payzeny: Paying negative Zeny (zeny=%d, account_id=%d, char_id=%d).\n", zeny, sd->status.account_id, sd->status.char_id);
		return 1;
	}

	if( sd->status.zeny < zeny )
		return 1; //Not enough.

	sd->status.zeny -= zeny;
	clif->updatestatus(sd,SP_ZENY);

	if (zeny > 0) {
		achievement->validate_zeny(sd, -zeny); // Achievements [Smokexyz/Hercules]
		logs->zeny(sd, type, tsd ? tsd : sd, -zeny);

		if (sd->state.showzeny) {
			char output[255];
			sprintf(output, msg_sd(sd, 885), zeny); // Removed %dz.
			clif_disp_onlyself(sd, output);
		}
	}

	return 0;
}

/**
 * Calculates leftover cashpoints and kafrapoints when buying an item from cashshop
 *
 * @param price     Price of the item.
 * @param points    Provided kafra points.
 *
 * @return points	Leftover kafra points.
 */
//Changed Kafrapoints calculation. [Normynator]
static int pc_paycash(struct map_session_data *sd, int price, int points)
{
	int cash;
	int mempoints;
	nullpo_retr(-1, sd);

	points = cap_value(points, -MAX_ZENY, MAX_ZENY); //prevent command UB
	if (price < 0 || points < 0) {
		ShowError("pc_paycash: Paying negative points (price=%d, points=%d, account_id=%d, char_id=%d).\n", price, points, sd->status.account_id, sd->status.char_id);
		return -2;
	}

	if (points > price) {
		ShowWarning("pc_paycash: More kafra points provided than needed (price=%d, points=%d, account_id=%d, char_id=%d).\n", price, points, sd->status.account_id, sd->status.char_id);
		points = points - price;
		mempoints = price;
		cash = 0;
	} else {
		cash = price - points;
		mempoints = points;
		points = 0;
	}

	if (sd->cashPoints < cash || sd->kafraPoints < mempoints) {
		ShowError("pc_paycash: Not enough points (cash=%d, kafra=%d) to cover the price (cash=%d, kafra=%d) (account_id=%d, char_id=%d).\n", sd->cashPoints, sd->kafraPoints, cash, points, sd->status.account_id, sd->status.char_id);
		return -1;
	}

	pc_setaccountreg(sd, script->add_variable("#CASHPOINTS"), sd->cashPoints - cash);
	pc_setaccountreg(sd, script->add_variable("#KAFRAPOINTS"), sd->kafraPoints - mempoints);

	if (battle_config.cashshop_show_points) {
		char output[128];
		sprintf(output, msg_sd(sd,504), points, cash, sd->kafraPoints, sd->cashPoints);
		clif_disp_onlyself(sd, output);
	}

	return points;
}

static int pc_getcash(struct map_session_data *sd, int cash, int points)
{
	char output[128];
	nullpo_retr(-1,sd);

	cash = cap_value(cash,-MAX_ZENY,MAX_ZENY); //prevent command UB
	points = cap_value(points,-MAX_ZENY,MAX_ZENY); //prevent command UB
	if( cash > 0 )
	{
		if( cash > MAX_ZENY-sd->cashPoints )
		{
			ShowWarning("pc_getcash: Cash point overflow (cash=%d, have cash=%d, account_id=%d, char_id=%d).\n", cash, sd->cashPoints, sd->status.account_id, sd->status.char_id);
			cash = MAX_ZENY-sd->cashPoints;
		}

		pc_setaccountreg(sd, script->add_variable("#CASHPOINTS"), sd->cashPoints+cash);

		if( battle_config.cashshop_show_points )
		{
			sprintf(output, msg_sd(sd,505), cash, sd->cashPoints);
			clif_disp_onlyself(sd, output);
		}
		return cash;
	}
	else if( cash < 0 )
	{
		ShowError("pc_getcash: Obtaining negative cash points (cash=%d, account_id=%d, char_id=%d).\n", cash, sd->status.account_id, sd->status.char_id);
		return -1;
	}

	if( points > 0 )
	{
		if( points > MAX_ZENY-sd->kafraPoints )
		{
			ShowWarning("pc_getcash: Kafra point overflow (points=%d, have points=%d, account_id=%d, char_id=%d).\n", points, sd->kafraPoints, sd->status.account_id, sd->status.char_id);
			points = MAX_ZENY-sd->kafraPoints;
		}

		pc_setaccountreg(sd, script->add_variable("#KAFRAPOINTS"), sd->kafraPoints+points);

		if( battle_config.cashshop_show_points )
		{
			sprintf(output, msg_sd(sd,506), points, sd->kafraPoints);
			clif_disp_onlyself(sd, output);
		}
		return points;
	}
	else if( points < 0 )
	{
		ShowError("pc_getcash: Obtaining negative kafra points (points=%d, account_id=%d, char_id=%d).\n", points, sd->status.account_id, sd->status.char_id);
		return -1;
	}
	return -2; //shouldn't happen but jsut in case
}

/*==========================================
 * Attempts to give zeny to player (sd)
 * tsd (optional) from who for log (if null take sd)
 *------------------------------------------*/
static int pc_getzeny(struct map_session_data *sd, int zeny, enum e_log_pick_type type, struct map_session_data *tsd)
{
	nullpo_retr(-1,sd);

	zeny = cap_value(zeny,-MAX_ZENY,MAX_ZENY); //prevent command UB
	if( zeny < 0 )
	{
		ShowError("pc_getzeny: Obtaining negative Zeny (zeny=%d, account_id=%d, char_id=%d).\n", zeny, sd->status.account_id, sd->status.char_id);
		return 1;
	}

	if( zeny > MAX_ZENY - sd->status.zeny )
		zeny = MAX_ZENY - sd->status.zeny;

	sd->status.zeny += zeny;
	clif->updatestatus(sd,SP_ZENY);

	if (zeny > 0) {
		achievement->validate_zeny(sd, zeny); // Achievements [Smokexyz/Hercules]
		logs->zeny(sd, type, tsd ? tsd : sd, zeny);

		if (sd->state.showzeny) {
			char output[255];
			sprintf(output, msg_sd(sd, 886), zeny); // Gained %dz.
			clif_disp_onlyself(sd, output);
		}
	}

	return 0;
}

/**
 * Searches for the specified item ID in inventory and return its inventory index.
 *
 * If the item is found, the returned value is guaranteed to be a valid index
 * (non-negative, smaller than MAX_INVENTORY).
 *
 * @param sd      Character to search on.
 * @param item_id The item ID to search.
 * @return the inventory index of the first instance of the requested item.
 * @retval INDEX_NOT_FOUND if the item wasn't found.
 */
static int pc_search_inventory(struct map_session_data *sd, int item_id)
{
	int i;
	nullpo_retr(INDEX_NOT_FOUND, sd);

	ARR_FIND(0, sd->status.inventorySize, i, sd->status.inventory[i].nameid == item_id && (sd->status.inventory[i].amount > 0 || item_id == 0));
	return (i < sd->status.inventorySize) ? i : INDEX_NOT_FOUND;
}

/*==========================================
 * Attempt to add a new item to inventory.
 * Return:
 * 0 = success
 * 1 = invalid itemid not found or negative amount
 * 2 = overweight
 * 3 = ?
 * 4 = no free place found
 * 5 = max amount reached
 * 6 = ?
 * 7 = stack limitation
 *------------------------------------------*/
static int pc_additem(struct map_session_data *sd, const struct item *item_data, int amount, e_log_pick_type log_type)
{
	struct item_data *data;
	int i;
	unsigned int w;

	nullpo_retr(1, sd);
	nullpo_retr(1, item_data);

	if( item_data->nameid <= 0 || amount <= 0 )
		return 1;
	if( amount > MAX_AMOUNT )
		return 5;

	data = itemdb->search(item_data->nameid);

	if( data->stack.inventory && amount > data->stack.amount )
	{// item stack limitation
		return 7;
	}

	w = data->weight*amount;
	if(sd->weight + w > sd->max_weight)
		return 2;

	if( item_data->bound ) {
		switch( (enum e_item_bound_type)item_data->bound ) {
			case IBT_CHARACTER:
			case IBT_ACCOUNT:
				break; /* no restrictions */
			case IBT_PARTY:
				if( !sd->status.party_id ) {
					ShowError("pc_additem: can't add party_bound item to character without party!\n");
					ShowError("pc_additem: %s - x%d %s (%d)\n",sd->status.name,amount,data->jname,data->nameid);
					return 7;/* need proper code? */
				}
				break;
			case IBT_GUILD:
				if( !sd->status.guild_id ) {
					ShowError("pc_additem: can't add guild_bound item to character without guild!\n");
					ShowError("pc_additem: %s - x%d %s (%d)\n",sd->status.name,amount,data->jname,data->nameid);
					return 7;/* need proper code? */
				}
				break;
			case IBT_NONE:
				break;
		}
	}

	i = sd->status.inventorySize;

	// Stackable | Non Rental
	if( itemdb->isstackable2(data) && item_data->expire_time == 0 ) {
		for (i = 0; i < sd->status.inventorySize; i++) {
			if( sd->status.inventory[i].nameid == item_data->nameid &&
			    sd->status.inventory[i].bound == item_data->bound &&
			    sd->status.inventory[i].expire_time == 0 &&
				sd->status.inventory[i].unique_id == item_data->unique_id &&
			    memcmp(&sd->status.inventory[i].card, &item_data->card, sizeof(item_data->card)) == 0 ) {
				if( amount > MAX_AMOUNT - sd->status.inventory[i].amount || ( data->stack.inventory && amount > data->stack.amount - sd->status.inventory[i].amount ) )
					return 5;
				sd->status.inventory[i].amount += amount;
				clif->additem(sd,i,amount,0);
				break;
			}
		}
	}

	if (i >= sd->status.inventorySize) {
		i = pc->search_inventory(sd,0);
		if (i == INDEX_NOT_FOUND)
			return 4;

		memcpy(&sd->status.inventory[i], item_data, sizeof(sd->status.inventory[0]));
		// clear equip and favorite fields first, just in case
		if( item_data->equip )
			sd->status.inventory[i].equip = 0;
		if( item_data->favorite )
			sd->status.inventory[i].favorite = 0;

		sd->status.inventory[i].amount = amount;
		sd->inventory_data[i] = data;
		clif->additem(sd,i,amount,0);

	}

	if( ( !itemdb->isstackable2(data) || data->flag.force_serial || data->type == IT_CASH) && !item_data->unique_id )
			sd->status.inventory[i].unique_id = itemdb->unique_id(sd);

	logs->pick_pc(sd, log_type, amount, &sd->status.inventory[i],sd->inventory_data[i]);

	achievement->validate_item_get(sd, sd->status.inventory[i].nameid, sd->status.inventory[i].amount); // Achievements [Smokexyz/Hercules]

	sd->weight += w;
	clif->updatestatus(sd,SP_WEIGHT);

	// auto-favorite
	if (data->flag.auto_favorite > 0) {
		sd->status.inventory[i].favorite = 1;
		clif->favorite_item(sd, i);
	}

	//Auto-equip
	if(data->flag.autoequip)
		pc->equipitem(sd, i, data->equip);

	/* rental item check */
	if (item_data->expire_time > 0) {
		if (time(NULL) > item_data->expire_time) {
			pc->rental_expire(sd, i);
		} else {
			int seconds = (int)(item_data->expire_time - time(NULL));
			clif->rental_time(sd->fd, sd->status.inventory[i].nameid, seconds);
			pc->inventory_rental_add(sd, seconds);
			if (data->rental_start_script != NULL)
				script->run_item_rental_start_script(sd, data, 0);
		}
	}
	quest->questinfo_refresh(sd);

	return 0;
}

/*==========================================
 * Remove an item at index n from inventory by amount.
 * Parameters :
 * @type
 *   1 : don't notify deletion
 *   2 : don't notify weight change
 * reason: @see enum delitem_reason
 * Return:
 *   0 = success
 *   1 = invalid itemid or negative amount
 *------------------------------------------*/
static int pc_delitem(struct map_session_data *sd, int n, int amount, int type, enum delitem_reason reason, e_log_pick_type log_type)
{
	nullpo_retr(1, sd);
	Assert_retr(1, n >= 0 && n < sd->status.inventorySize);

	if(sd->status.inventory[n].nameid==0 || amount <= 0 || sd->status.inventory[n].amount<amount || sd->inventory_data[n] == NULL)
		return 1;

	logs->pick_pc(sd, log_type, -amount, &sd->status.inventory[n],sd->inventory_data[n]);

	sd->status.inventory[n].amount -= amount;
	sd->weight -= sd->inventory_data[n]->weight*amount ;

	// It's here because the data would most likely get zeroed in following if [Hemagx]
	struct item_data *itd = sd->inventory_data[n];
	bool is_rental = (sd->status.inventory[n].expire_time > 0) ? true : false;

	if( sd->status.inventory[n].amount <= 0 ){
		if(sd->status.inventory[n].equip)
			pc->unequipitem(sd, n, PCUNEQUIPITEM_RECALC|PCUNEQUIPITEM_FORCE);
		memset(&sd->status.inventory[n],0,sizeof(sd->status.inventory[0]));
		sd->inventory_data[n] = NULL;
	}

	if (is_rental && itd->rental_end_script != NULL)
		script->run_item_rental_end_script(sd, itd, 0);

	if(!(type&1))
		clif->delitem(sd,n,amount,reason);
	if(!(type&2))
		clif->updatestatus(sd,SP_WEIGHT);
	quest->questinfo_refresh(sd);

	return 0;
}

/*==========================================
 * Attempt to drop an item.
 * Return:
 *   0 = fail
 *   1 = success
 *------------------------------------------*/
static int pc_dropitem(struct map_session_data *sd, int n, int amount)
{
	nullpo_retr(1, sd);

	if(n < 0 || n >= sd->status.inventorySize)
		return 0;

	if(amount <= 0)
		return 0;

	if(sd->status.inventory[n].nameid <= 0 ||
		sd->status.inventory[n].amount <= 0 ||
		sd->status.inventory[n].amount < amount ||
		sd->state.trading || sd->state.vending || sd->state.prevend ||
		!sd->inventory_data[n] //pc->delitem would fail on this case.
		)
		return 0;

	if( map->list[sd->bl.m].flag.nodrop ) {
		clif->message (sd->fd, msg_sd(sd,271)); // You can't drop items in this map
		return 0;
	}

	if( !pc->candrop(sd,&sd->status.inventory[n]) )
	{
		clif->message (sd->fd, msg_sd(sd,263)); // This item cannot be dropped.
		return 0;
	}

	if (!map->addflooritem(&sd->bl, &sd->status.inventory[n], amount, sd->bl.m, sd->bl.x, sd->bl.y, 0, 0, 0, 2, false))
		return 0;

	pc->delitem(sd, n, amount, 1, DELITEM_NORMAL, LOG_TYPE_PICKDROP_PLAYER);
	clif->dropitem(sd, n, amount);
	return 1;
}

/*==========================================
 * Attempt to pick up an item.
 * Return:
 *   0 = fail
 *   1 = success
 *------------------------------------------*/
static int pc_takeitem(struct map_session_data *sd, struct flooritem_data *fitem)
{
	int flag=0;
	int64 tick = timer->gettick();
	struct party_data *p=NULL;

	nullpo_ret(sd);
	nullpo_ret(fitem);

	if(!check_distance_bl(&fitem->bl, &sd->bl, 2) && sd->ud.skill_id!=BS_GREED)
		return 0; // Distance is too far

	if( pc_has_permission(sd,PC_PERM_DISABLE_PICK_UP) )
		return 0;

	if (sd->status.party_id)
		p = party->search(sd->status.party_id);

	if (fitem->first_get_charid > 0 && fitem->first_get_charid != sd->status.char_id) {
		struct map_session_data *first_sd = map->charid2sd(fitem->first_get_charid);
		if (DIFF_TICK(tick,fitem->first_get_tick) < 0) {
			if (!(p && p->party.item&1 &&
				first_sd && first_sd->status.party_id == sd->status.party_id
			))
				return 0;
		} else if (fitem->second_get_charid > 0 && fitem->second_get_charid != sd->status.char_id) {
			struct map_session_data *second_sd = map->charid2sd(fitem->second_get_charid);
			if (DIFF_TICK(tick, fitem->second_get_tick) < 0) {
				if (!(p && p->party.item&1 &&
					((first_sd && first_sd->status.party_id == sd->status.party_id) ||
					(second_sd && second_sd->status.party_id == sd->status.party_id))
				))
					return 0;
			} else if (fitem->third_get_charid > 0 && fitem->third_get_charid != sd->status.char_id) {
				struct map_session_data *third_sd = map->charid2sd(fitem->third_get_charid);
				if (DIFF_TICK(tick,fitem->third_get_tick) < 0) {
					if (!(p && p->party.item&1 &&
						((first_sd && first_sd->status.party_id == sd->status.party_id) ||
						(second_sd && second_sd->status.party_id == sd->status.party_id) ||
						(third_sd && third_sd->status.party_id == sd->status.party_id))
					))
						return 0;
				}
			}
		}
	}

	//This function takes care of giving the item to whoever should have it, considering party-share options.
	if ((flag = party->share_loot(p,sd,&fitem->item_data, fitem->first_get_charid))) {
		clif->additem(sd,0,0,flag);
		return 1;
	}

	//Display pickup animation.
	pc_stop_attack(sd);
	clif->takeitem(&sd->bl,&fitem->bl);
	map->clearflooritem(&fitem->bl);
	return 1;
}

/*==========================================
 * Check if item is usable.
 * Return:
 *   0 = no
 *   1 = yes
 *------------------------------------------*/
static int pc_isUseitem(struct map_session_data *sd, int n)
{
	struct item_data *item;
	int nameid;

	nullpo_ret(sd);
	Assert_ret(n >= 0 && n < sd->status.inventorySize);

	item = sd->inventory_data[n];
	nameid = sd->status.inventory[n].nameid;

	if( item == NULL )
		return 0;
	//Not consumable item
	if (!itemdb->is_item_usable(item))
		return 0;
	if( !item->script ) //if it has no script, you can't really consume it!
		return 0;

	if ((item->item_usage.flag&INR_SITTING) && (pc_issit(sd) == 1) && (pc_get_group_level(sd) < item->item_usage.override)) {
		clif->msgtable(sd, MSG_CANT_USE_WHEN_SITDOWN);
		//clif->messagecolor_self(sd->fd, COLOR_WHITE, msg_txt(1474));
		return 0; // You cannot use this item while sitting.
	}

	switch( nameid ) { // TODO: Is there no better way to handle this, other than hardcoding item IDs?
		case ITEMID_ANODYNE:
			if (map_flag_gvg2(sd->bl.m)) {
#if PACKETVER >= 20080311
				clif->skill_mapinfomessage(sd, 3);
#else
				clif->messagecolor_self(sd->fd, COLOR_CYAN, msg_sd(sd, 51));
#endif
				return 0;
			}
			break;

		case ITEMID_GIANT_FLY_WING: {
			struct party_data *p;

			if (!sd->status.party_id) {
#if PACKETVER >= 20061030
				clif->msgtable(sd, MSG_CANNOT_PARTYCALL);
#endif
				break;
			}

			if ((p = party->search(sd->status.party_id)) != NULL) {
				int i;
				int16 m;

				ARR_FIND(0, MAX_PARTY, i, p->data[i].sd == sd);

				if (i == MAX_PARTY || !p->party.member[i].leader) {
#if PACKETVER >= 20061030
					clif->msgtable(sd, MSG_CANNOT_PARTYCALL);
#endif
					break;
				}

				m = sd->bl.m;

				ARR_FIND(0, MAX_PARTY, i, p->data[i].sd && p->data[i].sd != sd && p->data[i].sd->bl.m == m);

				if (i == MAX_PARTY || pc_isdead(p->data[i].sd)) {
#if PACKETVER >= 20061030
					clif->msgtable(sd, MSG_NO_PARTYMEM_ON_THISMAP);
#endif
					break;
				}
			}
		}
		FALLTHROUGH
		case ITEMID_WING_OF_FLY:
		case ITEMID_N_FLY_WING:
		case ITEMID_C_WING_OF_FLY:
			if (map->list[sd->bl.m].flag.noteleport || map_flag_gvg2(sd->bl.m)) {
#if PACKETVER >= 20080311
				clif->skill_mapinfomessage(sd, 0);
#else
				clif->messagecolor_self(sd->fd, COLOR_RED, msg_sd(sd, 49));
#endif
				return 0;
			}
			/* Fall through */
		case ITEMID_WING_OF_BUTTERFLY:
		case ITEMID_N_BUTTERFLY_WING:
		case ITEMID_DUN_TELE_SCROLL1:
		case ITEMID_DUN_TELE_SCROLL2:
		case ITEMID_WOB_RUNE:     // Yellow Butterfly Wing
		case ITEMID_WOB_SCHWALTZ: // Green Butterfly Wing
		case ITEMID_WOB_RACHEL:   // Red Butterfly Wing
		case ITEMID_WOB_LOCAL:    // Blue Butterfly Wing
		case ITEMID_SIEGE_TELEPORT_SCROLL:
			if( sd->duel_group && !battle_config.duel_allow_teleport ) {
				clif->message(sd->fd, msg_sd(sd,863)); // "Duel: Can't use this item in duel."
				return 0;
			}
			if (nameid != ITEMID_WING_OF_FLY && nameid != ITEMID_GIANT_FLY_WING && map->list[sd->bl.m].flag.noreturn) {
#if PACKETVER >= 20080311
				clif->skill_mapinfomessage(sd, 0);
#else
				clif->messagecolor_self(sd->fd, COLOR_RED, msg_sd(sd, 49));
#endif
				return 0;
			}
			break;
		case ITEMID_BRANCH_OF_DEAD_TREE:
		case ITEMID_RED_POUCH_OF_SURPRISE:
		case ITEMID_BLOODY_DEAD_BRANCH:
		case ITEMID_PORING_BOX:
			if (map->list[sd->bl.m].flag.nobranch || map_flag_gvg2(sd->bl.m)) {
#if PACKETVER >= 20080311
				clif->skill_mapinfomessage(sd, 3);
#else
				clif->messagecolor_self(sd->fd, COLOR_CYAN, msg_sd(sd, 51));
#endif
				return 0;
			}
			break;

		// Mercenary Items
		case ITEMID_MERCENARY_RED_POTION:
		case ITEMID_MERCENARY_BLUE_POTION:
		case ITEMID_M_CENTER_POTION:
		case ITEMID_M_AWAKENING_POTION:
		case ITEMID_M_BERSERK_POTION:
			if( sd->md == NULL || sd->md->db == NULL )
				return 0;
			if (sd->md->sc.data[SC_BERSERK])
				return 0;
			if( nameid == ITEMID_M_AWAKENING_POTION && sd->md->db->lv < 40 )
				return 0;
			if( nameid == ITEMID_M_BERSERK_POTION && sd->md->db->lv < 80 )
				return 0;
			break;

		case ITEMID_NEURALIZER:
			if (!map->list[sd->bl.m].flag.reset) {
#if PACKETVER >= 20080311
				clif->skill_mapinfomessage(sd, 3);
#else
				clif->messagecolor_self(sd->fd, COLOR_CYAN, msg_sd(sd, 51));
#endif
				return 0;
			}
			break;
	}

	if( nameid >= ITEMID_BOW_MERCENARY_SCROLL1 && nameid <= ITEMID_SPEARMERCENARY_SCROLL10 && sd->md != NULL ) // Mercenary Scrolls
		return 0;

	if( item->package || item->group ) {
		if (pc_is90overweight(sd)) {
			clif->msgtable(sd, MSG_CANT_GET_ITEM_BECAUSE_WEIGHT);
			return 0;
		}
		if (!pc->inventoryblank(sd)) {
			clif->messagecolor_self(sd->fd, COLOR_RED, msg_sd(sd,1477));
			return 0;
		}
	}

	//Gender check
	if(item->sex != 2 && sd->status.sex != item->sex)
		return 0;
	//Required level check
	if (item->elv && sd->status.base_level < item->elv) {
#if PACKETVER >= 20100525
		clif->msgtable(sd, MSG_CANNOT_USE_ITEM_LEVEL);
#endif
		return 0;
	}

	if (item->elvmax && sd->status.base_level > item->elvmax) {
#if PACKETVER >= 20100525
		clif->msgtable(sd, MSG_CANNOT_USE_ITEM_LEVEL);
#endif
		return 0;
	}

	//Not equipable by class. [Skotlex]
	if (((1ULL << (sd->job & MAPID_BASEMASK)) & (item->class_base[(sd->job & JOBL_2_1) ? 1 : ((sd->job & JOBL_2_2) ? 2 : 0)])) == 0)
		return 0;

	//Not usable by upper class. [Haru]
	while( 1 ) {
		// Normal classes (no upper, no baby, no third classes)
		if ((item->class_upper & ITEMUPPER_NORMAL) != 0) {
			if ((sd->job & (JOBL_UPPER|JOBL_THIRD|JOBL_BABY)) == 0)
				break;
		}
		if ((item->class_upper & ITEMUPPER_UPPER) != 0) {
#ifdef RENEWAL
			// Upper classes (no third classes)
			if ((sd->job & JOBL_UPPER) != 0 && (sd->job&JOBL_THIRD) == 0)
				break;
#else
			//pre-re has no use for the extra, so we maintain the previous for backwards compatibility
			if ((sd->job & (JOBL_UPPER|JOBL_THIRD)) != 0)
				break;
#endif
		}
		// Baby classes (no third classes)
		if ((item->class_upper & ITEMUPPER_BABY) != 0) {
			if ((sd->job & JOBL_BABY) != 0 && (sd->job&JOBL_THIRD) == 0)
				break;
		}
		// Third classes (no upper, no baby classes)
		if ((item->class_upper & ITEMUPPER_THIRD) != 0) {
			if ((sd->job & JOBL_THIRD) != 0 && (sd->job & (JOBL_UPPER|JOBL_BABY)) == 0)
				break;
		}
		// Upper third classes
		if ((item->class_upper & ITEMUPPER_THIRDUPPER) != 0) {
			if ((sd->job & JOBL_THIRD) != 0 && (sd->job & JOBL_UPPER) != 0)
				break;
		}
		// Baby third classes
		if ((item->class_upper & ITEMUPPER_THIRDBABY) != 0) {
			if ((sd->job & JOBL_THIRD) != 0 && (sd->job & JOBL_BABY) != 0)
				break;
		}
		return 0;
	}

	return 1;
}

/*==========================================
 * Last checks to use an item.
 * Return:
 *   0 = fail
 *   1 = success
 *------------------------------------------*/
static int pc_useitem(struct map_session_data *sd, int n)
{
	int64 tick = timer->gettick();
	int amount, nameid, i;
	bool removeItem = false;

	nullpo_ret(sd);
	Assert_ret(n >= 0 && n < sd->status.inventorySize);

	if ((sd->npc_id != 0 && sd->state.using_megaphone == 0 && (sd->npc_item_flag & ITEMENABLEDNPC_CONSUME) == 0)
	    || (sd->state.workinprogress & 1) != 0) {
#if PACKETVER >= 20110308
		clif->msgtable(sd, MSG_BUSY);
#else
		clif->messagecolor_self(sd->fd, COLOR_WHITE, msg_sd(sd, 48));
#endif
		return 0;
	}

	if (battle_config.storage_use_item == 0 && sd->state.storage_flag != STORAGE_FLAG_CLOSED) {
		clif->messagecolor_self(sd->fd, COLOR_RED, msg_sd(sd, 1475));
		return 0; // You cannot use this item while storage is open.
	}

	if( sd->status.inventory[n].nameid <= 0 || sd->status.inventory[n].amount <= 0 )
		return 0;

	if (sd->block_action.useitem) // *pcblock script command
		return 0;

	if( !pc->isUseitem(sd,n) )
		return 0;

	// Store information for later use before it is lost (via pc->delitem) [Paradox924X]
	nameid = sd->inventory_data[n]->nameid;

	if (nameid == ITEMID_MEGAPHONE && ((sd->state.workinprogress & 2) != 0 || sd->state.using_megaphone != 0
					   || sd->npc_id != 0)) {
#if PACKETVER >= 20110308
		clif->msgtable(sd, MSG_BUSY);
#else
		clif->messagecolor_self(sd->fd, COLOR_WHITE, msg_sd(sd, 48));
#endif
		return 0;
	}

	if (nameid != ITEMID_NAUTHIZ && sd->sc.opt1 > 0 && sd->sc.opt1 != OPT1_STONEWAIT && sd->sc.opt1 != OPT1_BURNING)
		return 0;

	// Statuses that don't let the player use items
	if (sd->sc.count && (
		sd->sc.data[SC_BERSERK] ||
		(sd->sc.data[SC_GRAVITATION] && sd->sc.data[SC_GRAVITATION]->val3 == BCT_SELF) ||
		sd->sc.data[SC_TRICKDEAD] ||
		sd->sc.data[SC_HIDING] ||
		sd->sc.data[SC__SHADOWFORM] ||
		sd->sc.data[SC__INVISIBILITY] ||
		sd->sc.data[SC__MANHOLE] ||
		sd->sc.data[SC_KG_KAGEHUMI] ||
		sd->sc.data[SC_WHITEIMPRISON] ||
		sd->sc.data[SC_DEEP_SLEEP] ||
		sd->sc.data[SC_SATURDAY_NIGHT_FEVER] ||
		sd->sc.data[SC_COLD] ||
		sd->sc.data[SC_SUHIDE] ||
		pc_ismuted(&sd->sc, MANNER_NOITEM)
	    ))
		return 0;

	// Prevent mass item usage. [Skotlex]
	if (DIFF_TICK(sd->canuseitem_tick, tick) > 0)
		return 0;

	/* Items with delayed consume are not meant to work while in mounts except reins of mount(12622) */
	if (sd->inventory_data[n]->flag.delay_consume && nameid != ITEMID_BOARDING_HALTER) {
		if( sd->sc.data[SC_ALL_RIDING] )
			return 0;
		else if( pc_issit(sd) )
			return 0;
	}
	//Since most delay-consume items involve using a "skill-type" target cursor,
	//perform a skill-use check before going through. [Skotlex]
	//resurrection was picked as testing skill, as a non-offensive, generic skill, it will do.
	//FIXME: Is this really needed here? It'll be checked in unit.c after all and this prevents skill items using when silenced [Inkfish]
	if( sd->inventory_data[n]->flag.delay_consume && ( sd->ud.skilltimer != INVALID_TIMER /*|| !status->check_skilluse(&sd->bl, &sd->bl, ALL_RESURRECTION, 0)*/ ) )
		return 0;

	if (sd->inventory_data[n]->delay > 0) {
		ARR_FIND(0, MAX_ITEMDELAYS, i, sd->item_delay[i].nameid == nameid);
		if (i == MAX_ITEMDELAYS) /* item not found. try first empty now */
			ARR_FIND(0, MAX_ITEMDELAYS, i, sd->item_delay[i].nameid == 0);
		if (i < MAX_ITEMDELAYS) {
			if (sd->item_delay[i].nameid != 0) {// found
				if (DIFF_TICK(sd->item_delay[i].tick, tick) > 0) {
					int delay_tick = (int)(DIFF_TICK(sd->item_delay[i].tick, tick) / 1000);
#if PACKETVER >= 20101123
					clif->msgtable_num(sd, MSG_ITEM_REUSE_LIMIT_SECOND, delay_tick + 1); // [%d] seconds left until you can use
#else
					char delay_msg[100];
					sprintf(delay_msg, msg_sd(sd, 26), delay_tick + 1);
					clif->messagecolor_self(sd->fd, COLOR_YELLOW, delay_msg); // [%d] seconds left until you can use
#endif
					return 0; // Delay has not expired yet
				}
			} else {// not yet used item (all slots are initially empty)
				sd->item_delay[i].nameid = nameid;
			}
			if (!(nameid == ITEMID_BOARDING_HALTER && pc_hasmount(sd)))
				sd->item_delay[i].tick = tick + sd->inventory_data[n]->delay;
		} else {// should not happen
			ShowError("pc_useitem: Exceeded item delay array capacity! (nameid=%d, char_id=%d)\n", nameid, sd->status.char_id);
		}
		//clean up used delays so we can give room for more
		for(i = 0; i < MAX_ITEMDELAYS; i++) {
			if( DIFF_TICK(sd->item_delay[i].tick, tick) <= 0 ) {
				sd->item_delay[i].tick = 0;
				sd->item_delay[i].nameid = 0;
			}
		}
	}

	/* on restricted maps the item is consumed but the effect is not used */
	for(i = 0; i < map->list[sd->bl.m].zone->disabled_items_count; i++) {
		if( map->list[sd->bl.m].zone->disabled_items[i] == nameid ) {
#if PACKETVER >= 20080311
			clif->skill_mapinfomessage(sd, 3);
#else
			clif->messagecolor_self(sd->fd, COLOR_CYAN, msg_sd(sd, 50));
#endif
			if( battle_config.item_restricted_consumption_type && sd->status.inventory[n].expire_time == 0 ) {
				clif->useitemack(sd,n,sd->status.inventory[n].amount-1,true);

				// If Earth Spike Scroll is used while SC_EARTHSCROLL is active, there is a chance to don't consume the scroll. [Kenpachi]
				if ((nameid == ITEMID_EARTH_SCROLL_1_3 || nameid == ITEMID_EARTH_SCROLL_1_5)
				    && sd->sc.count > 0 && sd->sc.data[SC_EARTHSCROLL] != NULL
				    && rnd() % 100 > sd->sc.data[SC_EARTHSCROLL]->val2) {
					return 0;
				}

				pc->delitem(sd, n, 1, 1, DELITEM_NORMAL, LOG_TYPE_CONSUME);
			}
			return 0;
		}
	}

	//Dead Branch & Bloody Branch & Porings Box
	if( nameid == ITEMID_BRANCH_OF_DEAD_TREE || nameid == ITEMID_BLOODY_DEAD_BRANCH || nameid == ITEMID_PORING_BOX )
		logs->branch(sd);

	sd->itemid = sd->status.inventory[n].nameid;
	sd->itemindex = n;
	if(sd->catch_target_class != -1) //Abort pet catching.
		sd->catch_target_class = -1;

	amount = sd->status.inventory[n].amount;
	//Check if the item is to be consumed immediately [Skotlex]
	if (sd->inventory_data[n]->flag.delay_consume || sd->inventory_data[n]->flag.keepafteruse)
		clif->useitemack(sd,n,amount,true);
	else {
		if (sd->status.inventory[n].expire_time == 0) {
			clif->useitemack(sd, n, amount - 1, true);
			removeItem = true;
		} else {
			clif->useitemack(sd, n, 0, false);
		}
	}

	if (sd->status.inventory[n].card[0] == CARD0_CREATE
	 && pc->fame_rank(MakeDWord(sd->status.inventory[n].card[2], sd->status.inventory[n].card[3]), RANKTYPE_ALCHEMIST) > 0) {
		script->potion_flag = 2; // Famous player's potions have 50% more efficiency
		if (sd->sc.data[SC_SOULLINK] && sd->sc.data[SC_SOULLINK]->val2 == SL_ROGUE)
			script->potion_flag = 3; //Even more effective potions.
	}

	// Update item use time.
	sd->canuseitem_tick = tick + battle_config.item_use_interval;

	if (nameid == ITEMID_MEGAPHONE)
		sd->state.using_megaphone = 1;

	script->run_use_script(sd, sd->inventory_data[n], npc->fake_nd->bl.id);
	script->potion_flag = 0;

	// If Earth Spike Scroll is used while SC_EARTHSCROLL is active, there is a chance to don't consume the scroll. [Kenpachi]
	if ((nameid == ITEMID_EARTH_SCROLL_1_3 || nameid == ITEMID_EARTH_SCROLL_1_5) && sd->sc.count > 0
	    && sd->sc.data[SC_EARTHSCROLL] != NULL && rnd() % 100 > sd->sc.data[SC_EARTHSCROLL]->val2) {
		removeItem = false;
	}

	if (removeItem)
		pc->delitem(sd, n, 1, 1, DELITEM_NORMAL, LOG_TYPE_CONSUME);
	return 1;
}

/**
 * Unsets a character's currently processed auto-cast skill data.
 *
 * @param sd The character.
 *
 **/
static void pc_autocast_clear_current(struct map_session_data *sd)
{
	nullpo_retv(sd);

	sd->auto_cast_current.type = AUTOCAST_NONE;
	sd->auto_cast_current.skill_id = 0;
	sd->auto_cast_current.skill_lv = 0;
	sd->auto_cast_current.itemskill_conditions_checked = false;
	sd->auto_cast_current.itemskill_check_conditions = true;
	sd->auto_cast_current.itemskill_instant_cast = false;
	sd->auto_cast_current.itemskill_cast_on_self = false;
}

/**
 * Unsets a character's auto-cast related data.
 *
 * @param sd The character.
 *
 **/
static void pc_autocast_clear(struct map_session_data *sd)
{
	nullpo_retv(sd);

	pc->autocast_clear_current(sd);
	VECTOR_TRUNCATE(sd->auto_cast); // Truncate auto-cast vector.
}

/**
 * Sets a character's currently processed auto-cast skill data by comparing the skill ID.
 *
 * @param sd The character.
 * @param skill_id The skill ID to compare.
 *
 **/
static void pc_autocast_set_current(struct map_session_data *sd, int skill_id)
{
	nullpo_retv(sd);

	pc->autocast_clear_current(sd);

	for (int i = 0; i < VECTOR_LENGTH(sd->auto_cast); i++) {
		if (VECTOR_INDEX(sd->auto_cast, i).skill_id == skill_id) {
			sd->auto_cast_current = VECTOR_INDEX(sd->auto_cast, i);
			break;
		}
	}
}

/**
 * Removes a specific entry from a character's auto-cast vector.
 *
 * @param sd The character.
 * @param type The entry's auto-cast type.
 * @param skill_id The entry's skill ID.
 * @param skill_lv The entry's skill level.
 *
 **/
static void pc_autocast_remove(struct map_session_data *sd, enum autocast_type type, int skill_id, int skill_lv)
{
	nullpo_retv(sd);

	for (int i = 0; i < VECTOR_LENGTH(sd->auto_cast); i++) {
		if (VECTOR_INDEX(sd->auto_cast, i).type == type && VECTOR_INDEX(sd->auto_cast, i).skill_id == skill_id
		    && VECTOR_INDEX(sd->auto_cast, i).skill_lv == skill_lv) {
			VECTOR_ERASE(sd->auto_cast, i);
			break;
		}
	}
}

/*==========================================
 * Add item on cart for given index.
 * Return:
 *   0 = success
 *   1 = fail
 *------------------------------------------*/
static int pc_cart_additem(struct map_session_data *sd, struct item *item_data, int amount, e_log_pick_type log_type)
{
	struct item_data *data;
	int i,w;

	nullpo_retr(1, sd);
	nullpo_retr(1, item_data);

	if(item_data->nameid <= 0 || amount <= 0)
		return 1;
	data = itemdb->search(item_data->nameid);

	if( data->stack.cart && amount > data->stack.amount )
	{// item stack limitation
		return 1;
	}

	if (!itemdb_cancartstore(item_data, pc_get_group_level(sd)) || (item_data->bound > IBT_ACCOUNT && !pc_can_give_bound_items(sd))) {
		// Check item trade restrictions
		clif->message (sd->fd, msg_sd(sd,264)); // This item cannot be stored.
		return 1;/* TODO: there is no official response to this? */
	}

	if( (w = data->weight*amount) + sd->cart_weight > sd->cart_weight_max )
		return 1;

	i = MAX_CART;
	if( itemdb->isstackable2(data) && !item_data->expire_time )
	{
		ARR_FIND( 0, MAX_CART, i,
			sd->status.cart[i].nameid == item_data->nameid && sd->status.cart[i].bound == item_data->bound &&
			sd->status.cart[i].card[0] == item_data->card[0] && sd->status.cart[i].card[1] == item_data->card[1] &&
			sd->status.cart[i].card[2] == item_data->card[2] && sd->status.cart[i].card[3] == item_data->card[3] );
	};

	if( i < MAX_CART && item_data->unique_id == sd->status.cart[i].unique_id)
	{// item already in cart, stack it
		if( amount > MAX_AMOUNT - sd->status.cart[i].amount || ( data->stack.cart && amount > data->stack.amount - sd->status.cart[i].amount ) )
			return 2; // no room

		sd->status.cart[i].amount+=amount;
		clif->cart_additem(sd,i,amount,0);
	}
	else
	{// item not stackable or not present, add it
		ARR_FIND( 0, MAX_CART, i, sd->status.cart[i].nameid == 0 );
		if( i == MAX_CART )
			return 2; // no room

		memcpy(&sd->status.cart[i],item_data,sizeof(sd->status.cart[0]));
		sd->status.cart[i].amount=amount;
		sd->cart_num++;
		clif->cart_additem(sd,i,amount,0);
	}
	sd->status.cart[i].favorite = 0;/* clear */
	logs->pick_pc(sd, log_type, amount, &sd->status.cart[i],data);

	sd->cart_weight += w;
	clif->updatestatus(sd,SP_CARTINFO);

	return 0;
}

/*==========================================
 * Delete item on cart for given index.
 * Return:
 *   0 = success
 *   1 = fail
 *------------------------------------------*/
static int pc_cart_delitem(struct map_session_data *sd, int n, int amount, int type, e_log_pick_type log_type)
{
	struct item_data * data;
	nullpo_retr(1, sd);
	Assert_retr(1, n >= 0 && n < MAX_CART);

	if( sd->status.cart[n].nameid == 0 || sd->status.cart[n].amount < amount || !(data = itemdb->exists(sd->status.cart[n].nameid)) )
		return 1;

	logs->pick_pc(sd, log_type, -amount, &sd->status.cart[n],data);

	sd->status.cart[n].amount -= amount;
	sd->cart_weight -= data->weight*amount ;
	if(sd->status.cart[n].amount <= 0){
		memset(&sd->status.cart[n],0,sizeof(sd->status.cart[0]));
		sd->cart_num--;
	}
	if(!type) {
		clif->cart_delitem(sd,n,amount);
		clif->updatestatus(sd,SP_CARTINFO);
	}

	return 0;
}

/*==========================================
 * Transfer item from inventory to cart.
 * Return:
 *   0 = fail
 *   1 = succes
 *------------------------------------------*/
static int pc_putitemtocart(struct map_session_data *sd, int idx, int amount)
{
	struct item *item_data;
	int flag;

	nullpo_ret(sd);

	if (idx < 0 || idx >= sd->status.inventorySize) //Invalid index check [Skotlex]
		return 1;

	item_data = &sd->status.inventory[idx];

	if (item_data->nameid == 0 || amount < 1 || item_data->amount < amount || sd->state.vending || sd->state.prevend)
		return 1;

	if( (flag = pc->cart_additem(sd,item_data,amount,LOG_TYPE_NONE)) == 0 )
		return pc->delitem(sd, idx, amount, 0, DELITEM_TOCART, LOG_TYPE_NONE);

	return flag;
}

/*==========================================
 * Get number of item in cart.
 * Return:
 * -1 = itemid not found or no amount found
 * x  = remaining itemid on cart after get
 *------------------------------------------*/
static int pc_cartitem_amount(struct map_session_data *sd, int idx, int amount)
{
	struct item* item_data;

	nullpo_retr(-1, sd);
	Assert_retr(-1, idx >= 0 && idx < MAX_CART);

	item_data = &sd->status.cart[idx];
	if( item_data->nameid == 0 || item_data->amount == 0 )
		return -1;

	return item_data->amount - amount;
}

/*==========================================
 * Retrieve an item at index idx from cart.
 * Return:
 *   0 = player not found or (FIXME) succes (from pc->cart_delitem)
 *   1 = failure
 *------------------------------------------*/
static int pc_getitemfromcart(struct map_session_data *sd, int idx, int amount)
{
	struct item *item_data;
	int flag;

	nullpo_ret(sd);

	if (idx < 0 || idx >= MAX_CART) //Invalid index check [Skotlex]
		return 1;

	item_data=&sd->status.cart[idx];

	if (item_data->nameid == 0 || amount < 1 || item_data->amount < amount || sd->state.vending || sd->state.prevend)
		return 1;

	if ((flag = pc->additem(sd,item_data,amount,LOG_TYPE_NONE)) == 0)
		return pc->cart_delitem(sd,idx,amount,0,LOG_TYPE_NONE);

	return flag;
}

static void pc_bound_clear(struct map_session_data *sd, enum e_item_bound_type type)
{
	int i;

	nullpo_retv(sd);
	switch( type ) {
		/* both restricted to inventory */
		case IBT_PARTY:
		case IBT_CHARACTER:
			for (i = 0; i < sd->status.inventorySize; i++ ) {
				if( sd->status.inventory[i].bound == type ) {
					pc->delitem(sd, i, sd->status.inventory[i].amount, 0, DELITEM_SKILLUSE, LOG_TYPE_OTHER); // FIXME: is this the correct reason flag?
				}
			}
			break;
		case IBT_ACCOUNT:
			ShowError("Helllo! You reached pc_bound_clear for IBT_ACCOUNT, unfortunately no scenario was expected for this!\n");
			break;
		case IBT_GUILD: {
				struct guild_storage *gstor = idb_get(gstorage->db,sd->status.guild_id);

				for (i = 0; i < sd->status.inventorySize; i++ ) {
					if(sd->status.inventory[i].bound == type) {
						if( gstor )
							gstorage->additem(sd,gstor,&sd->status.inventory[i],sd->status.inventory[i].amount);
						pc->delitem(sd, i, sd->status.inventory[i].amount, 0, DELITEM_SKILLUSE, gstor ? LOG_TYPE_GSTORAGE : LOG_TYPE_OTHER); // FIXME: is this the correct reason flag?
					}
				}
				if( gstor )
					gstorage->close(sd);
			}
			break;
		case IBT_NONE:
			break;
	}
}
/*==========================================
 *  Display item stolen msg to player sd
 *------------------------------------------*/
static int pc_show_steal(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd = NULL, *tsd = NULL;
	int itemid;

	struct item_data *item=NULL;
	char output[100];

	sd=va_arg(ap,struct map_session_data *);
	itemid=va_arg(ap,int);

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_PC);
	tsd = BL_UCAST(BL_PC, bl);
	nullpo_ret(sd);

	if((item=itemdb->exists(itemid))==NULL)
		sprintf(output, msg_sd(sd, 887), sd->status.name, itemid); // %s stole an Unknown Item (id: %i).
	else
		sprintf(output, msg_sd(sd, 888), sd->status.name, item->jname); // %s stole %s.
	clif->message(tsd->fd, output);

	return 0;
}
/*==========================================
 * Steal an item from bl (mob).
 * Return:
 *   0 = fail
 *   1 = succes
 *------------------------------------------*/
static int pc_steal_item(struct map_session_data *sd, struct block_list *bl, uint16 skill_lv)
{
	int i,itemid,flag;
	int rate;
	struct status_data *sd_status, *md_status;
	struct mob_data *md = BL_CAST(BL_MOB, bl);
	struct item tmp_item;
	struct item_data *data = NULL;

	if (sd == NULL || md == NULL)
		return 0;

	if(md->state.steal_flag == UCHAR_MAX || ( md->sc.opt1 && md->sc.opt1 != OPT1_BURNING && md->sc.opt1 != OPT1_CRYSTALIZE ) ) //already stolen from / status change check
		return 0;

	sd_status= status->get_status_data(&sd->bl);
	md_status= status->get_status_data(bl);

	if (md->master_id || md_status->mode&MD_BOSS || mob_is_treasure(md) ||
		map->list[bl->m].flag.nomobloot || // check noloot map flag [Lorky]
		(battle_config.skill_steal_max_tries && //Reached limit of steal attempts. [Lupus]
			md->state.steal_flag++ >= battle_config.skill_steal_max_tries)
	) { //Can't steal from
		md->state.steal_flag = UCHAR_MAX;
		return 0;
	}

	// base skill success chance (percentual)
	rate = (sd_status->dex - md_status->dex)/2 + skill_lv*6 + 4 + sd->bonus.add_steal_rate;

	if( rate < 1 )
		return 0;

	// Try dropping one item, in the order from first to last possible slot.
	// Droprate is affected by the skill success rate.
	for (i = 0; i < MAX_MOB_DROP; i++) {
		if (md->db->dropitem[i].nameid == 0)
			continue;
		if ((data = itemdb->exists(md->db->dropitem[i].nameid)) == NULL)
			continue;
		if (data->type == IT_CARD)
			continue;
		if (rnd() % 10000 < apply_percentrate(md->db->dropitem[i].p, rate, 100))
			break;
	}
	if (i == MAX_MOB_DROP)
		return 0;

	itemid = md->db->dropitem[i].nameid;
	memset(&tmp_item,0,sizeof(tmp_item));
	tmp_item.nameid = itemid;
	tmp_item.amount = 1;
	tmp_item.identify = itemdb->isidentified2(data);
	flag = pc->additem(sd,&tmp_item,1,LOG_TYPE_PICKDROP_PLAYER);

	//TODO: Should we disable stealing when the item you stole couldn't be added to your inventory? Perhaps players will figure out a way to exploit this behaviour otherwise?
	md->state.steal_flag = UCHAR_MAX; //you can't steal from this mob any more

	if(flag) { //Failed to steal due to overweight
		clif->additem(sd,0,0,flag);
		return 0;
	}

	if(battle_config.show_steal_in_same_party)
		party->foreachsamemap(pc->show_steal,sd,AREA_SIZE,sd,tmp_item.nameid);

	//Logs items, Stolen from mobs [Lupus]
	logs->pick_mob(md, LOG_TYPE_STEAL, -1, &tmp_item, data);

	return 1;
}

/**
 * Steals zeny from a monster through the RG_STEALCOIN skill.
 *
 * @param sd       Source character
 * @param target   Target monster
 * @param skill_lv Skill Level
 *
 * @return Amount of stolen zeny (0 in case of failure)
 */
static int pc_steal_coin(struct map_session_data *sd, struct block_list *target, uint16 skill_lv)
{
	int rate;
	struct mob_data *md = BL_CAST(BL_MOB, target);

	if (sd == NULL || md == NULL)
		return 0;

	if (md->state.steal_coin_flag || md->sc.data[SC_STONE] || md->sc.data[SC_FREEZE] || md->status.mode&MD_BOSS)
		return 0;

	if (mob_is_treasure(md))
		return 0;

	rate = skill_lv * 10 + (sd->status.base_level - md->level) * 2 + sd->battle_status.dex / 2 + sd->battle_status.luk / 2;
	if(rnd()%1000 < rate) {
		int amount = md->level * skill_lv / 10 + md->level * 8 + rnd()%(md->level * 2 + 1); // mob_lv * skill_lv / 10 + random [mob_lv*8; mob_lv*10]

		pc->getzeny(sd, amount, LOG_TYPE_STEAL, NULL);
		md->state.steal_coin_flag = 1;
		return amount;
	}
	return 0;
}

/**
 * Sets a character's position.
 *
 * @param sd The related character.
 * @param map_index The target map's index.
 * @param x The target x-coordinate.
 * @param y The target y-coordinate.
 * @param clrtype The unit clear type, which should be used.
 * @retval 0 Success.
 * @retval 1 Invalid map index.
 * @retval 2 (unused) Map not in this map-server, and failed to locate alternative map-server.
 * @retval 3 No character data. (Parameter sd is a NULL pointer.)
 * @retval 4 Character is jailed.
 *
 **/
static int pc_setpos(struct map_session_data *sd, unsigned short map_index, int x, int y, enum clr_type clrtype)
{
	nullpo_retr(3, sd);

	int map_id = map->mapindex2mapid(map_index);

	if (map_index == 0 || !mapindex_id2name(map_index) || map_id == INDEX_NOT_FOUND) {
		ShowDebug("pc_setpos: Passed mapindex %d is invalid!\n", map_index);
		return 1;
	}
	Assert_retr(1, map_id >= 0);

	if (pc_isdead(sd)) { // Revive dead character before warping.
		pc->setstand(sd);
		pc->setrestartvalue(sd, 1);
	}

	if (map->list[map_id].flag.src4instance != 0) {
		bool stop = false;

		if (sd->instances != 0) {
			int i, j = 0;

			for (i = 0; i < sd->instances; i++) {
				if (sd->instance[i] >= 0) {
					ARR_FIND(0, instance->list[sd->instance[i]].num_map, j,
						 map->list[instance->list[sd->instance[i]].map[j]].instance_src_map == map_id
						 && !map->list[instance->list[sd->instance[i]].map[j]].custom_name);

					if (j != instance->list[sd->instance[i]].num_map)
						break;
				}
			}

			if (i != sd->instances) {
				map_id = instance->list[sd->instance[i]].map[j];
				map_index = map_id2index(map_id);
				stop = true;
			}
		}

		struct party_data *p = party->search(sd->status.party_id);

		if (!stop && sd->status.party_id != 0 && p != NULL && p->instances != 0) {
			int i, j = 0;

			for (i = 0; i < p->instances; i++) {
				if (p->instance[i] >= 0) {
					ARR_FIND(0, instance->list[p->instance[i]].num_map, j,
						 map->list[instance->list[p->instance[i]].map[j]].instance_src_map == map_id
						 && !map->list[instance->list[p->instance[i]].map[j]].custom_name);

					if (j != instance->list[p->instance[i]].num_map)
						break;
				}
			}

			if (i != p->instances) {
				map_id = instance->list[p->instance[i]].map[j];
				map_index = map_id2index(map_id);
				stop = true;
			}
		}

		if (!stop && sd->status.guild_id != 0 && sd->guild != NULL && sd->guild->instances != 0) {
			int i, j = 0;

			for (i = 0; i < sd->guild->instances; i++) {
				if (sd->guild->instance[i] >= 0) {
					ARR_FIND(0, instance->list[sd->guild->instance[i]].num_map, j,
						 map->list[instance->list[sd->guild->instance[i]].map[j]].instance_src_map == map_id
						 && !map->list[instance->list[sd->guild->instance[i]].map[j]].custom_name);

					if (j != instance->list[sd->guild->instance[i]].num_map)
						break;
				}
			}

			if (i != sd->guild->instances) {
				map_id = instance->list[sd->guild->instance[i]].map[j];
				map_index = map_id2index(map_id);
				//stop = true; Uncomment when adding new checks.
			}
		}
		Assert_retr(1, map_id >= 0);

		// We hit an instance. If empty we populate the spawn data.
		if (map->list[map_id].instance_id >= 0 && instance->list[map->list[map_id].instance_id].respawn.map == 0
		    && instance->list[map->list[map_id].instance_id].respawn.x == 0
		    && instance->list[map->list[map_id].instance_id].respawn.y == 0) {
			instance->list[map->list[map_id].instance_id].respawn.map = map_index;
			instance->list[map->list[map_id].instance_id].respawn.x = x;
			instance->list[map->list[map_id].instance_id].respawn.y = y;
		}
	}

	sd->state.changemap = (sd->mapindex != map_index) ? 1 : 0;
	sd->state.warping = 1;
	sd->state.workinprogress = 0;

	if (sd->state.changemap != 0) { // Miscellaneous map-changing settings.
		sd->state.pmap = sd->bl.m;

		for (int i = 0; i < VECTOR_LENGTH(sd->script_queues); i++) {
			struct script_queue *queue = script->queue(VECTOR_INDEX(sd->script_queues, i));

			if (queue != NULL && queue->event_mapchange[0] != '\0') {
				pc->setregstr(sd, script->add_variable("@Queue_Destination_Map$"), map->list[map_id].name);
				npc->event(sd, queue->event_mapchange, 0);
			}
		}

		if (map->list[map_id].cell == (struct mapcell *)0xdeadbeaf)
			map->cellfromcache(&map->list[map_id]);

		if (sd->sc.count != 0) { // Cancel some map related stuff.
			if (sd->sc.data[SC_JAILED] != NULL)
				return 4; // You may not get out!

			status_change_end(&sd->bl, SC_CASH_BOSS_ALARM, INVALID_TIMER);
			status_change_end(&sd->bl, SC_WARM, INVALID_TIMER);
			status_change_end(&sd->bl, SC_SUN_COMFORT, INVALID_TIMER);
			status_change_end(&sd->bl, SC_MOON_COMFORT, INVALID_TIMER);
			status_change_end(&sd->bl, SC_STAR_COMFORT, INVALID_TIMER);
			status_change_end(&sd->bl, SC_MIRACLE, INVALID_TIMER);
			status_change_end(&sd->bl, SC_NEUTRALBARRIER_MASTER, INVALID_TIMER); // Will later check if this is needed. [Rytech]
			status_change_end(&sd->bl, SC_NEUTRALBARRIER, INVALID_TIMER);
			status_change_end(&sd->bl, SC_STEALTHFIELD_MASTER, INVALID_TIMER);
			status_change_end(&sd->bl, SC_STEALTHFIELD, INVALID_TIMER);

			if (sd->sc.data[SC_KNOWLEDGE] != NULL) {
				struct status_change_entry *sce = sd->sc.data[SC_KNOWLEDGE];

				if (sce->timer != INVALID_TIMER)
					timer->delete(sce->timer, status->change_timer);

				sce->timer = timer->add(timer->gettick() + skill->get_time(SG_KNOWLEDGE, sce->val1),
							status->change_timer, sd->bl.id, SC_KNOWLEDGE);
			}

			status_change_end(&sd->bl, SC_PROPERTYWALK, INVALID_TIMER);
			status_change_end(&sd->bl, SC_CLOAKING, INVALID_TIMER);
			status_change_end(&sd->bl, SC_CLOAKINGEXCEED, INVALID_TIMER);
		}

		if ((battle_config.clear_unit_onwarp & BL_PC) != 0)
			skill->clear_unitgroup(&sd->bl);

		party->send_dot_remove(sd); // Minimap dot fix. [Kevin]
		guild->send_dot_remove(sd);
		bg->send_dot_remove(sd);

		// Make sure that vending is allowed here.
		if (sd->state.vending != 0 && map->list[map_id].flag.novending != 0) {
			clif->message(sd->fd, msg_sd(sd, 276)); // "You can't open a shop on this map"
			vending->close(sd);
		}

		if (sd->mapindex != 0 && map->list[sd->bl.m].channel != NULL) // Only if the character is already on a map.
			channel->leave(map->list[sd->bl.m].channel, sd);
	}

	if (x < 0 || x >= map->list[map_id].xs || y < 0 || y >= map->list[map_id].ys) { // Invalid coordinates. Randomize them.
		ShowError("pc_setpos: Attempt to place player %s (%d:%d) on invalid coordinates (%s-%d,%d)!\n",
			  sd->status.name, sd->status.account_id, sd->status.char_id,
			  mapindex_id2name(map_index), x, y);
		x = 0;
		y = 0;
	}

	if (x == 0 && y == 0) { // Pick a random walkable cell.
		do {
			x = rnd() % (map->list[map_id].xs - 2) + 1;
			y = rnd() % (map->list[map_id].ys - 2) + 1;
		} while(map->getcell(map_id, &sd->bl, x, y, CELL_CHKNOPASS) != 0);
	}

	if (sd->state.vending != 0 && map->getcell(map_id, &sd->bl, x, y, CELL_CHKNOVENDING) != 0) {
		clif->message(sd->fd, msg_sd(sd, 204)); // "You can't open a shop on this cell."
		vending->close(sd);
	}

	if (battle_config.player_warp_keep_direction == 0)
		sd->ud.dir = 0; // Make character facing north.

	if (sd->bl.prev != NULL) {
		unit->remove_map_pc(sd, clrtype);
		clif->changemap(sd, map_id, x, y); // [MouseJstr]
	} else if (sd->state.active != 0) {
		sd->state.rewarp = 1; // Tag character for re-warping after map-loading is done. [Skotlex]
	}

	if (sd->status.guild_id > 0
		&& (map->list[map_id].flag.gvg_castle == 1 || map->list[sd->bl.m].flag.gvg_castle == 1)) {
		// If coming from or going to a guild castle, recalculate HP/SP regen [Hemagx]
		status->calc_regen(&sd->bl, &sd->battle_status, &sd->regen);
		status->calc_regen_rate(&sd->bl, &sd->regen);
	}

	sd->mapindex = map_index;
	sd->bl.m = map_id;
	sd->bl.x = x;
	sd->bl.y = y;
	sd->ud.to_x = x;
	sd->ud.to_y = y;

	if (sd->status.pet_id > 0 && sd->pd != NULL && sd->pd->pet.intimate > PET_INTIMACY_NONE) {
		sd->pd->bl.m = map_id;
		sd->pd->bl.x = x;
		sd->pd->bl.y = y;
		sd->pd->ud.to_x = x;
		sd->pd->ud.to_y = y;
		sd->pd->ud.dir = sd->ud.dir;
	}

	if (homun_alive(sd->hd)) {
		sd->hd->bl.m = map_id;
		sd->hd->bl.x = x;
		sd->hd->bl.y = y;
		sd->hd->ud.to_x = x;
		sd->hd->ud.to_y = y;
		sd->hd->ud.dir = sd->ud.dir;
	}

	if (sd->md != NULL) {
		sd->md->bl.m = map_id;
		sd->md->bl.x = x;
		sd->md->bl.y = y;
		sd->md->ud.to_x = x;
		sd->md->ud.to_y = y;
		sd->md->ud.dir = sd->ud.dir;
	}

	// Given autotrades have no clients. You have to trigger this manually, otherwise they get stuck in memory limbo. (bugreport:7495)
	if (sd->state.autotrade != 0)
		clif->pLoadEndAck(0, sd);

	return 0;
}

/*==========================================
 * Warp player sd to random location on current map.
 * May fail if no walkable cell found (1000 attempts).
 * Return:
 *   0 = fail or FIXME success (from pc->setpos)
 *   x(1|2) = fail
 *------------------------------------------*/
static int pc_randomwarp(struct map_session_data *sd, enum clr_type type)
{
	int x,y,i=0;
	int16 m;

	nullpo_ret(sd);

	m=sd->bl.m;

	if (map->list[sd->bl.m].flag.noteleport) //Teleport forbidden
		return 0;

	do {
		x=rnd()%(map->list[m].xs-2)+1;
		y=rnd()%(map->list[m].ys-2)+1;
	} while (map->getcell(m, &sd->bl, x, y, CELL_CHKNOPASS) && (i++) < 1000 );

	if (i < 1000)
		return pc->setpos(sd,map_id2index(sd->bl.m),x,y,type);

	return 0;
}

/*==========================================
 * Records a memo point at sd's current position
 * pos - entry to replace, (-1: shift oldest entry out)
 *------------------------------------------*/
static int pc_memo(struct map_session_data *sd, int pos)
{
	int skill_lv;

	nullpo_ret(sd);

	// check mapflags
	if( sd->bl.m >= 0 && (map->list[sd->bl.m].flag.nomemo || map->list[sd->bl.m].flag.nowarpto) && !pc_has_permission(sd, PC_PERM_WARP_ANYWHERE) ) {
		clif->skill_mapinfomessage(sd, 1); // "Saved point cannot be memorized."
		return 0;
	}

	// check inputs
	if( pos < -1 || pos >= MAX_MEMOPOINTS )
		return 0; // invalid input

	// check required skill level
	skill_lv = pc->checkskill(sd, AL_WARP);
	if( skill_lv < 1 ) {
		clif->skill_memomessage(sd,2); // "You haven't learned Warp."
		return 0;
	}
	if( skill_lv < 2 || skill_lv - 2 < pos ) {
		clif->skill_memomessage(sd,1); // "Skill Level is not high enough."
		return 0;
	}

	if( pos == -1 )
	{
		int i;
		// prevent memo-ing the same map multiple times
		ARR_FIND( 0, MAX_MEMOPOINTS, i, sd->status.memo_point[i].map == map_id2index(sd->bl.m) );
		memmove(&sd->status.memo_point[1], &sd->status.memo_point[0], (min(i,MAX_MEMOPOINTS-1))*sizeof(struct point));
		pos = 0;
	}

	sd->status.memo_point[pos].map = map_id2index(sd->bl.m);
	sd->status.memo_point[pos].x = sd->bl.x;
	sd->status.memo_point[pos].y = sd->bl.y;

	clif->skill_memomessage(sd, 0);

	return 1;
}

//
// Skills
//
/*==========================================
 * Return player sd skill_lv learned for given skill
 *------------------------------------------*/
static int pc_checkskill(struct map_session_data *sd, uint16 skill_id)
{
	int index = 0;
	if(sd == NULL) return 0;
	if( skill_id >= GD_SKILLBASE && skill_id < GD_MAX ) {
		struct guild *g;

		if( sd->status.guild_id>0 && (g=sd->guild)!=NULL)
			return guild->checkskill(g,skill_id);
		return 0;
	} else if(!(index = skill->get_index(skill_id)) || index >= ARRAYLENGTH(sd->status.skill) ) {
		ShowError("pc_checkskill: Invalid skill id %d (char_id=%d).\n", skill_id, sd->status.char_id);
		return 0;
	}

	if(sd->status.skill[index].id == skill_id)
		return (sd->status.skill[index].lv);

	return 0;
}
static int pc_checkskill2(struct map_session_data *sd, uint16 index)
{
	if (sd == NULL)
		return 0;
	if (index >= MAX_SKILL_DB) {
		ShowError("pc_checkskill: Invalid skill index %d (char_id=%d).\n", index, sd->status.char_id);
		return 0;
	}
	if( skill->dbs->db[index].nameid >= GD_SKILLBASE && skill->dbs->db[index].nameid < GD_MAX ) {
		struct guild *g;

		if( sd->status.guild_id>0 && (g=sd->guild)!=NULL)
			return guild->checkskill(g,skill->dbs->db[index].nameid);
		return 0;
	}
	if(sd->status.skill[index].id == skill->dbs->db[index].nameid)
		return (sd->status.skill[index].lv);

	return 0;
}

/*==========================================
 * Chk if we still have the correct weapon to continue the skill (actually status)
 * If not ending it
 * Return
 *   0 - No status found or all done
 *------------------------------------------*/
static int pc_checkallowskill(struct map_session_data *sd)
{
	const enum sc_type scw_list[] = {
		SC_TWOHANDQUICKEN,
		SC_ONEHANDQUICKEN,
		SC_AURABLADE,
		SC_PARRYING,
		SC_SPEARQUICKEN,
		SC_ADRENALINE,
		SC_ADRENALINE2,
		SC_DANCING,
		SC_GS_GATLINGFEVER,
#ifdef RENEWAL
		SC_LKCONCENTRATION,
		SC_EDP,
#endif
		SC_FEARBREEZE,
		SC_EXEEDBREAK,
	};
	const enum sc_type scs_list[] = {
		SC_AUTOGUARD,
		SC_DEFENDER,
		SC_REFLECTSHIELD,
		SC_LG_REFLECTDAMAGE
	};
	int i;
	nullpo_ret(sd);

	if(!sd->sc.count)
		return 0;

	for (i = 0; i < ARRAYLENGTH(scw_list); i++) {
		// Skills requiring specific weapon types
		if( scw_list[i] == SC_DANCING && !battle_config.dancing_weaponswitch_fix )
			continue;
		if( sd->sc.data[scw_list[i]]
		 && !pc_check_weapontype(sd,skill->get_weapontype(status->sc2skill(scw_list[i]))))
			status_change_end(&sd->bl, scw_list[i], INVALID_TIMER);
	}

	if(sd->sc.data[SC_STRUP] && sd->weapontype != W_FIST)
		// Spurt requires bare hands (feet, in fact xD)
		status_change_end(&sd->bl, SC_STRUP, INVALID_TIMER);

	if (!sd->has_shield) { // Skills requiring a shield
		for (i = 0; i < ARRAYLENGTH(scs_list); i++)
			if(sd->sc.data[scs_list[i]])
				status_change_end(&sd->bl, scs_list[i], INVALID_TIMER);
	}
	return 0;
}

/*==========================================
 * Return equiped itemid? on player sd at pos
 * Return
 * -1 : mean nothing equiped
 * idx : (this index could be used in inventory to found item_data)
 *------------------------------------------*/
static int pc_checkequip(struct map_session_data *sd, int pos)
{
	int i;

	nullpo_retr(-1, sd);

	for(i=0;i<EQI_MAX;i++){
		if(pos & pc->equip_pos[i])
			return sd->equip_index[i];
	}

	return -1;
}

/**
 * Get the skill current cooldown for player.
 * (get the db base cooldown for skill + player specific cooldown)
 * @param sd : player pointer
 * @param id : skill id
 * @param lv : skill lv
 * @return player skill cooldown
 */
int pc_get_skill_cooldown(struct map_session_data *sd, uint16 skill_id, uint16 skill_lv)
{
	nullpo_ret(sd);
	Assert_ret(skill_id > 0 && skill_lv > 0);

	if (skill_id == SJ_NOVAEXPLOSING) {
		const struct status_change *sc = status->get_sc(&sd->bl);
		if (sc != NULL && sc->data[SC_DIMENSION] != NULL)
			return 0;
	}

	int i;
	int cooldown = skill->get_cooldown(skill_id, skill_lv);

	ARR_FIND(0, ARRAYLENGTH(sd->skillcooldown), i, sd->skillcooldown[i].id == skill_id);

	if (i < ARRAYLENGTH(sd->skillcooldown))
		cooldown += sd->skillcooldown[i].val;

	return max(0, cooldown);
}

/*==========================================
 * Convert's from the client's lame Job ID system
 * to the map server's 'makes sense' system. [Skotlex]
 *------------------------------------------*/
static int pc_jobid2mapid(int class)
{
	switch (class) {
		case JOB_KAGEROU:
		case JOB_OBORO:                 return MAPID_KAGEROUOBORO;
		case JOB_CLOWN:
		case JOB_GYPSY:                 return MAPID_CLOWNGYPSY;
		case JOB_BABY_KAGEROU:
		case JOB_BABY_OBORO:            return MAPID_BABY_KAGEROUOBORO;
		case JOB_BARD:
		case JOB_DANCER:                return MAPID_BARDDANCER;
		case JOB_BABY_BARD:
		case JOB_BABY_DANCER:           return MAPID_BABY_BARDDANCER;
		case JOB_MINSTREL:
		case JOB_WANDERER:              return MAPID_MINSTRELWANDERER;
		case JOB_MINSTREL_T:
		case JOB_WANDERER_T:            return MAPID_MINSTRELWANDERER_T;
		case JOB_BABY_MINSTREL:
		case JOB_BABY_WANDERER:         return MAPID_BABY_MINSTRELWANDERER;
#define ENUM_VALUE(name, id) case JOB_ ## name: return MAPID_ ## name;
#include "common/class.h"
#undef ENUM_VALUE
#define ENUM_VALUE(name, id) case JOB_ ## name: return -1;
#include "common/class_hidden.h"
#undef ENUM_VALUE
		default:
			return -1;
	}
}

//Reverts the map-style class id to the client-style one.
static int pc_mapid2jobid(unsigned int class, int sex)
{
	switch (class) {
		case MAPID_KAGEROUOBORO:          return sex ? JOB_KAGEROU : JOB_OBORO;
		case MAPID_BARDDANCER:            return sex ? JOB_BARD : JOB_DANCER;
		case MAPID_CLOWNGYPSY:            return sex ? JOB_CLOWN : JOB_GYPSY;
		case MAPID_BABY_KAGEROUOBORO:     return sex ? JOB_BABY_KAGEROU : JOB_BABY_OBORO;
		case MAPID_BABY_BARDDANCER:       return sex ? JOB_BABY_BARD : JOB_BABY_DANCER;
		case MAPID_MINSTRELWANDERER:      return sex ? JOB_MINSTREL : JOB_WANDERER;
		case MAPID_MINSTRELWANDERER_T:    return sex ? JOB_MINSTREL_T : JOB_WANDERER_T;
		case MAPID_BABY_MINSTRELWANDERER: return sex ? JOB_BABY_MINSTREL : JOB_BABY_WANDERER;
#define ENUM_VALUE(name, id) case MAPID_ ## name: return JOB_ ## name;
#include "common/class.h"
#undef ENUM_VALUE
		default:
			return -1;
	}
}

/*====================================================
 * This function return the name of the job (by [Yor])
 *----------------------------------------------------*/
static const char *pc_job_name(int class)
{
	switch (class) {
	case JOB_NOVICE:   // 550
	case JOB_SWORDMAN: // 551
	case JOB_MAGE:     // 552
	case JOB_ARCHER:   // 553
	case JOB_ACOLYTE:  // 554
	case JOB_MERCHANT: // 555
	case JOB_THIEF:    // 556
		return msg_txt(550 - JOB_NOVICE + class);

	case JOB_KNIGHT:     // 557
	case JOB_PRIEST:     // 558
	case JOB_WIZARD:     // 559
	case JOB_BLACKSMITH: // 560
	case JOB_HUNTER:     // 561
	case JOB_ASSASSIN:   // 562
		return msg_txt(557 - JOB_KNIGHT + class);

	case JOB_KNIGHT2:
		return msg_txt(557);

	case JOB_CRUSADER:  // 563
	case JOB_MONK:      // 564
	case JOB_SAGE:      // 565
	case JOB_ROGUE:     // 566
	case JOB_ALCHEMIST: // 567
	case JOB_BARD:      // 568
	case JOB_DANCER:    // 569
		return msg_txt(563 - JOB_CRUSADER + class);

	case JOB_CRUSADER2:
		return msg_txt(563);

	case JOB_WEDDING:      // 570
	case JOB_SUPER_NOVICE: // 571
	case JOB_GUNSLINGER:   // 572
	case JOB_NINJA:        // 573
	case JOB_XMAS:         // 574
		return msg_txt(570 - JOB_WEDDING + class);

	case JOB_SUMMER:
		return msg_txt(621);

	case JOB_NOVICE_HIGH:   // 575
	case JOB_SWORDMAN_HIGH: // 576
	case JOB_MAGE_HIGH:     // 577
	case JOB_ARCHER_HIGH:   // 578
	case JOB_ACOLYTE_HIGH:  // 579
	case JOB_MERCHANT_HIGH: // 580
	case JOB_THIEF_HIGH:    // 581
		return msg_txt(575 - JOB_NOVICE_HIGH + class);

	case JOB_LORD_KNIGHT:    // 582
	case JOB_HIGH_PRIEST:    // 583
	case JOB_HIGH_WIZARD:    // 584
	case JOB_WHITESMITH:     // 585
	case JOB_SNIPER:         // 586
	case JOB_ASSASSIN_CROSS: // 587
		return msg_txt(582 - JOB_LORD_KNIGHT + class);

	case JOB_LORD_KNIGHT2:
		return msg_txt(582);

	case JOB_PALADIN:   // 588
	case JOB_CHAMPION:  // 589
	case JOB_PROFESSOR: // 590
	case JOB_STALKER:   // 591
	case JOB_CREATOR:   // 592
	case JOB_CLOWN:     // 593
	case JOB_GYPSY:     // 594
		return msg_txt(588 - JOB_PALADIN + class);

	case JOB_PALADIN2:
		return msg_txt(588);

	case JOB_BABY:          // 595
	case JOB_BABY_SWORDMAN: // 596
	case JOB_BABY_MAGE:     // 597
	case JOB_BABY_ARCHER:   // 598
	case JOB_BABY_ACOLYTE:  // 599
	case JOB_BABY_MERCHANT: // 600
	case JOB_BABY_THIEF:    // 601
		return msg_txt(595 - JOB_BABY + class);

	case JOB_BABY_KNIGHT:     // 602
	case JOB_BABY_PRIEST:     // 603
	case JOB_BABY_WIZARD:     // 604
	case JOB_BABY_BLACKSMITH: // 605
	case JOB_BABY_HUNTER:     // 606
	case JOB_BABY_ASSASSIN:   // 607
		return msg_txt(602 - JOB_BABY_KNIGHT + class);

	case JOB_BABY_KNIGHT2:
		return msg_txt(602);

	case JOB_BABY_CRUSADER:  // 608
	case JOB_BABY_MONK:      // 609
	case JOB_BABY_SAGE:      // 610
	case JOB_BABY_ROGUE:     // 611
	case JOB_BABY_ALCHEMIST: // 612
	case JOB_BABY_BARD:      // 613
	case JOB_BABY_DANCER:    // 614
		return msg_txt(608 - JOB_BABY_CRUSADER + class);

	case JOB_BABY_CRUSADER2:
		return msg_txt(608);

	case JOB_SUPER_BABY:
		return msg_txt(615);

	case JOB_TAEKWON:
		return msg_txt(616);
	case JOB_STAR_GLADIATOR:
	case JOB_STAR_GLADIATOR2:
		return msg_txt(617);
	case JOB_SOUL_LINKER:
		return msg_txt(618);

	case JOB_GANGSI:         // 622
	case JOB_DEATH_KNIGHT:   // 623
	case JOB_DARK_COLLECTOR: // 624
		return msg_txt(622 - JOB_GANGSI + class);

	case JOB_RUNE_KNIGHT:      // 625
	case JOB_WARLOCK:          // 626
	case JOB_RANGER:           // 627
	case JOB_ARCH_BISHOP:      // 628
	case JOB_MECHANIC:         // 629
	case JOB_GUILLOTINE_CROSS: // 630
		return msg_txt(625 - JOB_RUNE_KNIGHT + class);

	case JOB_RUNE_KNIGHT_T:      // 656
	case JOB_WARLOCK_T:          // 657
	case JOB_RANGER_T:           // 658
	case JOB_ARCH_BISHOP_T:      // 659
	case JOB_MECHANIC_T:         // 660
	case JOB_GUILLOTINE_CROSS_T: // 661
		return msg_txt(656 - JOB_RUNE_KNIGHT_T + class);

	case JOB_ROYAL_GUARD:   // 631
	case JOB_SORCERER:      // 632
	case JOB_MINSTREL:      // 633
	case JOB_WANDERER:      // 634
	case JOB_SURA:          // 635
	case JOB_GENETIC:       // 636
	case JOB_SHADOW_CHASER: // 637
		return msg_txt(631 - JOB_ROYAL_GUARD + class);

	case JOB_ROYAL_GUARD_T:   // 662
	case JOB_SORCERER_T:      // 663
	case JOB_MINSTREL_T:      // 664
	case JOB_WANDERER_T:      // 665
	case JOB_SURA_T:          // 666
	case JOB_GENETIC_T:       // 667
	case JOB_SHADOW_CHASER_T: // 668
		return msg_txt(662 - JOB_ROYAL_GUARD_T + class);

	case JOB_RUNE_KNIGHT2:
		return msg_txt(625);

	case JOB_RUNE_KNIGHT_T2:
		return msg_txt(656);

	case JOB_ROYAL_GUARD2:
		return msg_txt(631);

	case JOB_ROYAL_GUARD_T2:
		return msg_txt(662);

	case JOB_RANGER2:
		return msg_txt(627);

	case JOB_RANGER_T2:
		return msg_txt(658);

	case JOB_MECHANIC2:
		return msg_txt(629);

	case JOB_MECHANIC_T2:
		return msg_txt(660);

	case JOB_BABY_RUNE:     // 638
	case JOB_BABY_WARLOCK:  // 639
	case JOB_BABY_RANGER:   // 640
	case JOB_BABY_BISHOP:   // 641
	case JOB_BABY_MECHANIC: // 642
	case JOB_BABY_CROSS:    // 643
	case JOB_BABY_GUARD:    // 644
	case JOB_BABY_SORCERER: // 645
	case JOB_BABY_MINSTREL: // 646
	case JOB_BABY_WANDERER: // 647
	case JOB_BABY_SURA:     // 648
	case JOB_BABY_GENETIC:  // 649
	case JOB_BABY_CHASER:   // 650
		return msg_txt(638 - JOB_BABY_RUNE + class);

	case JOB_BABY_RUNE2:
		return msg_txt(638);

	case JOB_BABY_GUARD2:
		return msg_txt(644);

	case JOB_BABY_RANGER2:
		return msg_txt(640);

	case JOB_BABY_MECHANIC2:
		return msg_txt(642);

	case JOB_SUPER_NOVICE_E: // 651
	case JOB_SUPER_BABY_E:   // 652
		return msg_txt(651 - JOB_SUPER_NOVICE_E + class);

	case JOB_KAGEROU: // 653
	case JOB_OBORO:   // 654
		return msg_txt(653 - JOB_KAGEROU + class);

	case JOB_REBELLION:
		return msg_txt(655);

	case JOB_SUMMONER:
		return msg_txt(669);

	case JOB_BABY_SUMMONER:
		return msg_txt(670);

	case JOB_BABY_NINJA:
		return msg_txt(671);

	case JOB_BABY_KAGEROU:
	case JOB_BABY_OBORO:
		return msg_txt(672 - JOB_BABY_KAGEROU + class);

	case JOB_BABY_TAEKWON:
		return msg_txt(674);

	case JOB_BABY_STAR_GLADIATOR:
	case JOB_BABY_STAR_GLADIATOR2:
		return msg_txt(675);

	case JOB_BABY_SOUL_LINKER:
		return msg_txt(676);

	case JOB_BABY_GUNSLINGER:
		return msg_txt(677);

	case JOB_BABY_REBELLION:
		return msg_txt(678);

	case JOB_STAR_EMPEROR:
		return msg_txt(679);

	case JOB_BABY_STAR_EMPEROR:
		return msg_txt(680);

	case JOB_SOUL_REAPER:
		return msg_txt(681);

	case JOB_BABY_SOUL_REAPER:
		return msg_txt(682);


	default:
		return msg_txt(620); // "Unknown Job"
	}
}

static int pc_check_job_name(const char *name)
{
	int i, len;
	struct {
		const char *name;
		int id;
	} names[] = {
		{ "Novice", JOB_NOVICE },
		{ "Swordsman", JOB_SWORDMAN },
		{ "Magician", JOB_MAGE },
		{ "Archer", JOB_ARCHER },
		{ "Acolyte", JOB_ACOLYTE },
		{ "Merchant", JOB_MERCHANT },
		{ "Thief", JOB_THIEF },
		{ "Knight", JOB_KNIGHT },
		{ "Priest", JOB_PRIEST },
		{ "Wizard", JOB_WIZARD },
		{ "Blacksmith", JOB_BLACKSMITH },
		{ "Hunter", JOB_HUNTER },
		{ "Assassin", JOB_ASSASSIN },
		{ "Crusader", JOB_CRUSADER },
		{ "Monk", JOB_MONK },
		{ "Sage", JOB_SAGE },
		{ "Rogue", JOB_ROGUE },
		{ "Alchemist", JOB_ALCHEMIST },
		{ "Bard", JOB_BARD },
		{ "Dancer", JOB_DANCER },
		{ "Super_Novice", JOB_SUPER_NOVICE },
		{ "Gunslinger", JOB_GUNSLINGER },
		{ "Ninja", JOB_NINJA },
		{ "Novice_High", JOB_NOVICE_HIGH },
		{ "Swordsman_High", JOB_SWORDMAN_HIGH },
		{ "Magician_High", JOB_MAGE_HIGH },
		{ "Archer_High", JOB_ARCHER_HIGH },
		{ "Acolyte_High", JOB_ACOLYTE_HIGH },
		{ "Merchant_High", JOB_MERCHANT_HIGH },
		{ "Thief_High", JOB_THIEF_HIGH },
		{ "Lord_Knight", JOB_LORD_KNIGHT },
		{ "High_Priest", JOB_HIGH_PRIEST },
		{ "High_Wizard", JOB_HIGH_WIZARD },
		{ "Whitesmith", JOB_WHITESMITH },
		{ "Sniper", JOB_SNIPER },
		{ "Assassin_Cross", JOB_ASSASSIN_CROSS },
		{ "Paladin", JOB_PALADIN },
		{ "Champion", JOB_CHAMPION },
		{ "Professor", JOB_PROFESSOR },
		{ "Stalker", JOB_STALKER },
		{ "Creator", JOB_CREATOR },
		{ "Clown", JOB_CLOWN },
		{ "Gypsy", JOB_GYPSY },
		{ "Baby_Novice", JOB_BABY },
		{ "Baby_Swordsman", JOB_BABY_SWORDMAN },
		{ "Baby_Magician", JOB_BABY_MAGE },
		{ "Baby_Archer", JOB_BABY_ARCHER },
		{ "Baby_Acolyte", JOB_BABY_ACOLYTE },
		{ "Baby_Merchant", JOB_BABY_MERCHANT },
		{ "Baby_Thief", JOB_BABY_THIEF },
		{ "Baby_Knight", JOB_BABY_KNIGHT },
		{ "Baby_Priest", JOB_BABY_PRIEST },
		{ "Baby_Wizard", JOB_BABY_WIZARD },
		{ "Baby_Blacksmith", JOB_BABY_BLACKSMITH },
		{ "Baby_Hunter", JOB_BABY_HUNTER },
		{ "Baby_Assassin", JOB_BABY_ASSASSIN },
		{ "Baby_Crusader", JOB_BABY_CRUSADER },
		{ "Baby_Monk", JOB_BABY_MONK },
		{ "Baby_Sage", JOB_BABY_SAGE },
		{ "Baby_Rogue", JOB_BABY_ROGUE },
		{ "Baby_Alchemist", JOB_BABY_ALCHEMIST },
		{ "Baby_Bard", JOB_BABY_BARD },
		{ "Baby_Dancer", JOB_BABY_DANCER },
		{ "Baby_Ninja", JOB_BABY_NINJA },
		{ "Baby_Summoner", JOB_BABY_SUMMONER },
		{ "Baby_Kagerou", JOB_BABY_KAGEROU },
		{ "Baby_Oboro", JOB_BABY_OBORO },
		{ "Baby_Taekwon", JOB_BABY_TAEKWON },
		{ "Baby_Soul_Linker", JOB_BABY_SOUL_LINKER },
		{ "Baby_Gunslinger", JOB_BABY_GUNSLINGER },
		{ "Super_Baby", JOB_SUPER_BABY },
		{ "Baby_Star_Gladiator", JOB_BABY_STAR_GLADIATOR },
		{ "Baby_Star_Emperor", JOB_BABY_STAR_EMPEROR },
		{ "Baby_Soul_Reaper", JOB_BABY_SOUL_REAPER },
		{ "Taekwon", JOB_TAEKWON },
		{ "Star_Gladiator", JOB_STAR_GLADIATOR },
		{ "Soul_Linker", JOB_SOUL_LINKER },
		{ "Gangsi", JOB_GANGSI },
		{ "Death_Knight", JOB_DEATH_KNIGHT },
		{ "Dark_Collector", JOB_DARK_COLLECTOR },
		{ "Rune_Knight", JOB_RUNE_KNIGHT },
		{ "Warlock", JOB_WARLOCK },
		{ "Ranger", JOB_RANGER },
		{ "Arch_Bishop", JOB_ARCH_BISHOP },
		{ "Mechanic", JOB_MECHANIC },
		{ "Guillotine_Cross", JOB_GUILLOTINE_CROSS },
		{ "Star_Emperor", JOB_STAR_EMPEROR },
		{ "Soul_Reaper", JOB_SOUL_REAPER },
		{ "Rune_Knight_Trans", JOB_RUNE_KNIGHT_T },
		{ "Warlock_Trans", JOB_WARLOCK_T },
		{ "Ranger_Trans", JOB_RANGER_T },
		{ "Arch_Bishop_Trans", JOB_ARCH_BISHOP_T },
		{ "Mechanic_Trans", JOB_MECHANIC_T },
		{ "Guillotine_Cross_Trans", JOB_GUILLOTINE_CROSS_T },
		{ "Royal_Guard", JOB_ROYAL_GUARD },
		{ "Sorcerer", JOB_SORCERER },
		{ "Minstrel", JOB_MINSTREL },
		{ "Wanderer", JOB_WANDERER },
		{ "Sura", JOB_SURA },
		{ "Genetic", JOB_GENETIC },
		{ "Shadow_Chaser", JOB_SHADOW_CHASER },
		{ "Royal_Guard_Trans", JOB_ROYAL_GUARD_T },
		{ "Sorcerer_Trans", JOB_SORCERER_T },
		{ "Minstrel_Trans", JOB_MINSTREL_T },
		{ "Wanderer_Trans", JOB_WANDERER_T },
		{ "Sura_Trans", JOB_SURA_T },
		{ "Genetic_Trans", JOB_GENETIC_T },
		{ "Shadow_Chaser_Trans", JOB_SHADOW_CHASER_T },
		{ "Baby_Rune_Knight", JOB_BABY_RUNE },
		{ "Baby_Warlock", JOB_BABY_WARLOCK },
		{ "Baby_Ranger", JOB_BABY_RANGER },
		{ "Baby_Arch_Bishop", JOB_BABY_BISHOP },
		{ "Baby_Mechanic", JOB_BABY_MECHANIC },
		{ "Baby_Guillotine_Cross", JOB_BABY_CROSS },
		{ "Baby_Royal_Guard", JOB_BABY_GUARD },
		{ "Baby_Sorcerer", JOB_BABY_SORCERER },
		{ "Baby_Minstrel", JOB_BABY_MINSTREL },
		{ "Baby_Wanderer", JOB_BABY_WANDERER },
		{ "Baby_Sura", JOB_BABY_SURA },
		{ "Baby_Genetic", JOB_BABY_GENETIC },
		{ "Baby_Shadow_Chaser", JOB_BABY_CHASER },
		{ "Baby_Rebellion", JOB_BABY_REBELLION },
		{ "Expanded_Super_Novice", JOB_SUPER_NOVICE_E },
		{ "Expanded_Super_Baby", JOB_SUPER_BABY_E },
		{ "Kagerou", JOB_KAGEROU },
		{ "Oboro", JOB_OBORO },
		{ "Rebellion", JOB_REBELLION },
		{ "Summoner", JOB_SUMMONER },
	};

	nullpo_retr(-1, name);
	len = ARRAYLENGTH(names);

	ARR_FIND(0, len, i, strcmpi(names[i].name, name) == 0);

	if ( i == len )
		return -1;

	return names[i].id;
}

static int pc_follow_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd;
	struct block_list *tbl;

	sd = map->id2sd(id);
	nullpo_ret(sd);

	if (sd->followtimer != tid) {
		ShowError("pc_follow_timer %d != %d\n",sd->followtimer,tid);
		sd->followtimer = INVALID_TIMER;
		return 0;
	}

	sd->followtimer = INVALID_TIMER;
	tbl = map->id2bl(sd->followtarget);

	if (tbl == NULL || pc_isdead(sd) || status->isdead(tbl)) {
		pc->stop_following(sd);
		return 0;
	}

	// either player or target is currently detached from map blocks (could be teleporting),
	// but still connected to this map, so we'll just increment the timer and check back later
	if (sd->bl.prev != NULL && tbl->prev != NULL
	 && sd->ud.skilltimer == INVALID_TIMER && sd->ud.attacktimer == INVALID_TIMER && sd->ud.walktimer == INVALID_TIMER
	) {
		if((sd->bl.m == tbl->m) && unit->can_reach_bl(&sd->bl,tbl, AREA_SIZE, 0, NULL, NULL)) {
			if (!check_distance_bl(&sd->bl, tbl, 5))
				unit->walk_tobl(&sd->bl, tbl, 5, 0);
		} else
			pc->setpos(sd, map_id2index(tbl->m), tbl->x, tbl->y, CLR_TELEPORT);
	}
	sd->followtimer = timer->add(
		tick + 1000, // increase time a bit to loosen up map's load
		pc->follow_timer, sd->bl.id, 0);
	return 0;
}

static int pc_stop_following(struct map_session_data *sd)
{
	nullpo_ret(sd);

	if (sd->followtimer != INVALID_TIMER) {
		timer->delete(sd->followtimer,pc->follow_timer);
		sd->followtimer = INVALID_TIMER;
	}
	sd->followtarget = -1;
	sd->ud.target_to = 0;

	unit->stop_walking(&sd->bl, STOPWALKING_FLAG_FIXPOS);

	return 0;
}

static int pc_follow(struct map_session_data *sd, int target_id)
{
	struct block_list *bl = map->id2bl(target_id);
	nullpo_retr(1, sd);
	if (bl == NULL /*|| bl->type != BL_PC*/)
		return 1;
	if (sd->followtimer != INVALID_TIMER)
		pc->stop_following(sd);

	sd->followtarget = target_id;
	pc->follow_timer(INVALID_TIMER, timer->gettick(), sd->bl.id, 0);

	return 0;
}

static int pc_checkbaselevelup(struct map_session_data *sd)
{
	uint64 next = pc->nextbaseexp(sd);

	nullpo_ret(sd);
	if (!next || sd->status.base_exp < next)
		return 0;

	do {
		int status_points = 0;
		sd->status.base_exp -= next;
		//Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1. [Skotlex]
		if(!battle_config.multi_level_up && sd->status.base_exp > next-1)
			sd->status.base_exp = next-1;

		status_points = pc->gets_status_point(sd->status.base_level);
		sd->status.base_level++;
		sd->status.status_point += status_points;

	} while ((next=pc->nextbaseexp(sd)) > 0 && sd->status.base_exp >= next);

	if (battle_config.pet_lv_rate && sd->pd) //<Skotlex> update pet's level
		status_calc_pet(sd->pd,SCO_NONE);

	clif->updatestatus(sd,SP_STATUSPOINT);
	clif->updatestatus(sd,SP_BASELEVEL);
	clif->updatestatus(sd,SP_BASEEXP);
	clif->updatestatus(sd,SP_NEXTBASEEXP);
	status_calc_pc(sd,SCO_FORCE);
	status_percent_heal(&sd->bl,100,100);

	pc->checkbaselevelup_sc(sd);
	clif->misceffect(&sd->bl,0);
	npc->script_event(sd, NPCE_BASELVUP); //LORDALFA - LVLUPEVENT

	if (sd->status.party_id)
		party->send_levelup(sd);

	pc->baselevelchanged(sd);

	quest->questinfo_refresh(sd);

	achievement->validate_stats(sd, SP_BASELEVEL, sd->status.base_level);

	return 1;
}

static void pc_checkbaselevelup_sc(struct map_session_data *sd)
{
	nullpo_retv(sd);

	if ((sd->job & MAPID_UPPERMASK) == MAPID_SUPER_NOVICE) {
		sc_start(NULL, &sd->bl, skill->get_sc_type(PR_KYRIE), 100, 1, skill->get_time(PR_KYRIE, 1), PR_KYRIE);
		sc_start(NULL, &sd->bl, skill->get_sc_type(PR_IMPOSITIO), 100, 1, skill->get_time(PR_IMPOSITIO, 1), PR_IMPOSITIO);
		sc_start(NULL, &sd->bl, skill->get_sc_type(PR_MAGNIFICAT), 100, 1, skill->get_time(PR_MAGNIFICAT, 1), PR_MAGNIFICAT);
		sc_start(NULL, &sd->bl, skill->get_sc_type(PR_GLORIA), 100, 1, skill->get_time(PR_GLORIA, 1), PR_GLORIA);
		sc_start(NULL, &sd->bl, skill->get_sc_type(PR_SUFFRAGIUM), 100, 1, skill->get_time(PR_SUFFRAGIUM, 1), PR_SUFFRAGIUM);
		if (sd->state.snovice_dead_flag)
			sd->state.snovice_dead_flag = 0; //Reenable steelbody resurrection on dead.
	} else if ((sd->job & MAPID_BASEMASK) == MAPID_TAEKWON) {
		sc_start(NULL, &sd->bl, skill->get_sc_type(AL_INCAGI), 100, 10, 600000, AL_INCAGI);
		sc_start(NULL, &sd->bl, skill->get_sc_type(AL_BLESSING), 100, 10, 600000, AL_BLESSING);
	}
}

static void pc_baselevelchanged(struct map_session_data *sd)
{
	int i;
	nullpo_retv(sd);
	for( i = 0; i < EQI_MAX; i++ ) {
		if( sd->equip_index[i] >= 0 ) {
			if (sd->inventory_data[sd->equip_index[i]]->elvmax != 0 && sd->status.base_level > sd->inventory_data[ sd->equip_index[i] ]->elvmax)
				pc->unequipitem(sd, sd->equip_index[i], PCUNEQUIPITEM_RECALC|PCUNEQUIPITEM_FORCE);
		}
	}
}

static int pc_checkjoblevelup(struct map_session_data *sd)
{
	uint64 next = pc->nextjobexp(sd);

	nullpo_ret(sd);
	if(!next || sd->status.job_exp < next)
		return 0;

	do {
		sd->status.job_exp -= next;
		//Kyoki pointed out that the max overcarry exp is the exp needed for the previous level -1. [Skotlex]
		if(!battle_config.multi_level_up && sd->status.job_exp > next-1)
			sd->status.job_exp = next-1;

		sd->status.job_level++;
		sd->status.skill_point++;

	} while ((next=pc->nextjobexp(sd)) > 0 && sd->status.job_exp >= next);

	clif->updatestatus(sd,SP_JOBLEVEL);
	clif->updatestatus(sd,SP_JOBEXP);
	clif->updatestatus(sd,SP_NEXTJOBEXP);
	clif->updatestatus(sd,SP_SKILLPOINT);
	status_calc_pc(sd,SCO_FORCE);
	clif->misceffect(&sd->bl,1);
	if (pc->checkskill(sd, SG_DEVIL) && !pc->nextjobexp(sd))
		clif->status_change(&sd->bl, status->get_sc_icon(SC_DEVIL1), status->get_sc_relevant_bl_types(SC_DEVIL1), 1, 0, 0, 0, 1); //Permanent blind effect from SG_DEVIL.

	npc->script_event(sd, NPCE_JOBLVUP);

	quest->questinfo_refresh(sd);

	achievement->validate_stats(sd, SP_BASELEVEL, sd->status.job_level);

	return 1;
}

/**
 * Alters EXP based on self bonuses that do not get shared with the party
 **/
static void pc_calcexp(struct map_session_data *sd, uint64 *base_exp, uint64 *job_exp, struct block_list *src)
{
	int buff_ratio = 0, buff_job_ratio = 0, race_ratio = 0, pk_ratio = 0;
	int64 jexp, bexp;

	nullpo_retv(sd);
	nullpo_retv(base_exp);
	nullpo_retv(job_exp);

	jexp = *job_exp;
	bexp = *base_exp;

	if (src != NULL) {
		const struct status_data *st = status->get_status_data(src);

#ifdef RENEWAL_EXP //should happen first before we caluclate any modifiers
		if (src->type == BL_MOB) {
			const struct mob_data *md = BL_UCAST(BL_MOB, src);
			int re_mod;
			re_mod = pc->level_penalty_mod(md->level - sd->status.base_level, md->status.race, md->status.mode, 1);
			jexp = apply_percentrate64(jexp, re_mod, 100);
			bexp = apply_percentrate64(bexp, re_mod, 100);
		}
#endif

		//Race modifier
		if (sd->expaddrace[st->race])
			race_ratio += sd->expaddrace[st->race];
		race_ratio += sd->expaddrace[(st->mode&MD_BOSS) ? RC_BOSS : RC_NONBOSS];
	}


	//PK modifier
	/* this doesn't exist in Aegis, instead there's a CrazyKiller check which double all EXP from this point */
	if (battle_config.pk_mode && status->get_lv(src) - sd->status.base_level >= 20)
		pk_ratio += 15; // pk_mode additional exp if monster >20 levels [Valaris]


	//Buffs modifier
	if (sd->sc.data[SC_CASH_PLUSEXP]) {
		buff_job_ratio += sd->sc.data[SC_CASH_PLUSEXP]->val1;
		buff_ratio += sd->sc.data[SC_CASH_PLUSEXP]->val1;
	}
	if (sd->sc.data[SC_OVERLAPEXPUP]) {
		buff_job_ratio  += sd->sc.data[SC_OVERLAPEXPUP]->val1;
		buff_ratio += sd->sc.data[SC_OVERLAPEXPUP]->val1;
	}
	if (sd->sc.data[SC_CASH_PLUSONLYJOBEXP])
		buff_job_ratio += sd->sc.data[SC_CASH_PLUSONLYJOBEXP]->val1;

	//Applying Race and PK modifier First then Premium (Perment modifier) and finally buff modifier
	jexp += apply_percentrate64(jexp, race_ratio, 100);
	jexp += apply_percentrate64(jexp, pk_ratio, 100);

	bexp += apply_percentrate64(bexp, race_ratio, 100);
	bexp += apply_percentrate64(bexp, pk_ratio, 100);


	if (sd->status.mod_exp != 100) {
		jexp = apply_percentrate64(jexp, sd->status.mod_exp, 100);
		bexp = apply_percentrate64(bexp, sd->status.mod_exp, 100);
	}

	bexp += apply_percentrate64(bexp, buff_ratio, 100);
	jexp += apply_percentrate64(jexp, buff_ratio + buff_job_ratio, 100);

	*job_exp = cap_value(jexp, 1, UINT64_MAX);
	*base_exp = cap_value(bexp, 1, UINT64_MAX);
}

/**
 * Gives a determined EXP amount to sd and calculates remaining EXP for next level
 * @param src if is NULL no bonuses are taken into account
 * @param is_quest Used to let client know that the EXP was from a quest (clif->displayexp) PACKETVER >= 20091027
 * @retval true success
 **/
static bool pc_gainexp(struct map_session_data *sd, struct block_list *src, uint64 base_exp, uint64 job_exp, bool is_quest)
{
	float nextbp = 0, nextjp = 0;
	uint64 nextb = 0, nextj = 0;
	nullpo_ret(sd);

	if (sd->bl.prev == NULL || pc_isdead(sd))
		return false;

	if (!battle_config.pvp_exp && map->list[sd->bl.m].flag.pvp)  // [MouseJstr]
		return false; // no exp on pvp maps

	if (pc_has_permission(sd,PC_PERM_DISABLE_EXP))
		return false;

	if (src)
		pc->calcexp(sd, &base_exp, &job_exp, src);

	if (sd->status.guild_id > 0)
		base_exp -= guild->payexp(sd, base_exp);

	nextb = pc->nextbaseexp(sd);
	nextj = pc->nextjobexp(sd);

	if (sd->state.showexp || battle_config.max_exp_gain_rate) {
		if (nextb > 0)
			nextbp = (float) base_exp / (float) nextb;
		if (nextj > 0)
			nextjp = (float) job_exp / (float) nextj;

		if (battle_config.max_exp_gain_rate) {
			if (nextbp > battle_config.max_exp_gain_rate/1000.) {
				//Note that this value should never be greater than the original
				//base_exp, therefore no overflow checks are needed. [Skotlex]
				base_exp = (uint64)(battle_config.max_exp_gain_rate / 1000. * nextb);
				if (sd->state.showexp)
					nextbp = (float) base_exp / (float) nextb;
			}
			if (nextjp > battle_config.max_exp_gain_rate/1000.) {
				job_exp = (uint64)(battle_config.max_exp_gain_rate / 1000. * nextj);
				if (sd->state.showexp)
					nextjp = (float) job_exp / (float) nextj;
			}
		}
	}

	// Cap exp to the level up requirement of the previous level when you are at max level,
	// otherwise cap at UINT_MAX (this is required for some S. Novice bonuses). [Skotlex]
	if (base_exp) {
		nextb = nextb ? UINT64_MAX : pc->thisbaseexp(sd);
		if (sd->status.base_exp > nextb - base_exp)
			sd->status.base_exp = nextb;
		else
			sd->status.base_exp += base_exp;
		pc->checkbaselevelup(sd);
		clif->updatestatus(sd, SP_BASEEXP);
	}

	if (job_exp) {
		nextj = nextj ? UINT64_MAX : pc->thisjobexp(sd);
		if (sd->status.job_exp > nextj - job_exp)
			sd->status.job_exp = nextj;
		else
			sd->status.job_exp += job_exp;
		pc->checkjoblevelup(sd);
		clif->updatestatus(sd, SP_JOBEXP);
	}

#if PACKETVER >= 20091027
	if(base_exp)
		clif->displayexp(sd, base_exp, SP_BASEEXP, is_quest);
	if(job_exp)
		clif->displayexp(sd, job_exp,  SP_JOBEXP, is_quest);
#endif

	if(sd->state.showexp) {
		char output[256];
		sprintf(output,
			msg_sd(sd, 889), // Experience Gained Base:%llu (%.2f%%) Job:%llu (%.2f%%)
			(unsigned long long)base_exp, nextbp * 100.0f, (unsigned long long)job_exp, nextjp * 100.0f);
		clif_disp_onlyself(sd, output);
	}

	// Share master EXP to homunculus
	if (sd->hd != NULL && battle_config.hom_bonus_exp_from_master > 0) {
		homun->gainexp(sd->hd, apply_percentrate((int)base_exp, battle_config.hom_bonus_exp_from_master, 100));
	}

	return true;
}

/*==========================================
 * Returns max level for this character.
 *------------------------------------------*/
static int pc_maxbaselv(const struct map_session_data *sd)
{
	nullpo_ret(sd);

	const struct class_exp_group *group = pc->dbs->class_exp_table[pc->class2idx(sd->status.class)][CLASS_EXP_TABLE_BASE];
	nullpo_ret(group);
	return group->max_level;
}

static int pc_maxjoblv(const struct map_session_data *sd)
{
	nullpo_ret(sd);

	const struct class_exp_group *group = pc->dbs->class_exp_table[pc->class2idx(sd->status.class)][CLASS_EXP_TABLE_JOB];
	nullpo_ret(group);
	return group->max_level;
}

/*==========================================
 * base level exp lookup.
 *------------------------------------------*/

//Base exp needed for next level.
static uint64 pc_nextbaseexp(const struct map_session_data *sd)
{
	const struct class_exp_group *exp_group = NULL;

	nullpo_ret(sd);

	if (sd->status.base_level >= pc->maxbaselv(sd) || sd->status.base_level <= 0)
		return 0;

	exp_group = pc->dbs->class_exp_table[pc->class2idx(sd->status.class)][CLASS_EXP_TABLE_BASE];

	nullpo_ret(exp_group);

	return VECTOR_INDEX(exp_group->exp, sd->status.base_level >= exp_group->max_level ? 0 : sd->status.base_level - 1);
}

//Base exp needed for this level.
static uint64 pc_thisbaseexp(const struct map_session_data *sd)
{
	const struct class_exp_group *exp_group = NULL;

	nullpo_ret(sd);

	if (sd->status.base_level > pc->maxbaselv(sd) || sd->status.base_level <= 1)
		return 0;

	exp_group = pc->dbs->class_exp_table[pc->class2idx(sd->status.class)][CLASS_EXP_TABLE_BASE];

	nullpo_ret(exp_group);

	return VECTOR_INDEX(exp_group->exp, sd->status.base_level - 2);
}

/*==========================================
 * job level exp lookup
 * Return:
 *   0 = not found
 *   x = exp for level
 *------------------------------------------*/

//Job exp needed for next level.
static uint64 pc_nextjobexp(const struct map_session_data *sd)
{
	const struct class_exp_group *exp_group = NULL;

	nullpo_ret(sd);

	if (sd->status.job_level >= pc->maxjoblv(sd) || sd->status.job_level <= 0)
		return 0;

	exp_group = pc->dbs->class_exp_table[pc->class2idx(sd->status.class)][CLASS_EXP_TABLE_JOB];

	nullpo_ret(exp_group);

	return VECTOR_INDEX(exp_group->exp, sd->status.job_level >= exp_group->max_level ? 0 : sd->status.job_level - 1);
}

//Job exp needed for this level.
static uint64 pc_thisjobexp(const struct map_session_data *sd)
{
	const struct class_exp_group *exp_group = NULL;

	nullpo_ret(sd);

	if (sd->status.job_level > pc->maxjoblv(sd) || sd->status.job_level <= 1)
		return 0;

	exp_group = pc->dbs->class_exp_table[pc->class2idx(sd->status.class)][CLASS_EXP_TABLE_JOB];

	nullpo_ret(exp_group);

	return VECTOR_INDEX(exp_group->exp, sd->status.job_level - 2);
}

/// Returns the value of the specified stat.
static int pc_getstat(struct map_session_data *sd, int type)
{
	nullpo_retr(-1, sd);

	switch( type ) {
		case SP_STR: return sd->status.str;
		case SP_AGI: return sd->status.agi;
		case SP_VIT: return sd->status.vit;
		case SP_INT: return sd->status.int_;
		case SP_DEX: return sd->status.dex;
		case SP_LUK: return sd->status.luk;
		default:
			return -1;
	}
}

/// Sets the specified stat to the specified value.
/// Returns the new value.
static int pc_setstat(struct map_session_data *sd, int type, int val)
{
	nullpo_retr(-1, sd);

	switch( type ) {
		case SP_STR: sd->status.str = val; break;
		case SP_AGI: sd->status.agi = val; break;
		case SP_VIT: sd->status.vit = val; break;
		case SP_INT: sd->status.int_ = val; break;
		case SP_DEX: sd->status.dex = val; break;
		case SP_LUK: sd->status.luk = val; break;
		default:
			return -1;
	}

 	achievement->validate_stats(sd, type, val); // Achievements [Smokexyz/Hercules]

	return val;
}

// Calculates the number of status points PC gets when leveling up (from level to level+1)
static int pc_gets_status_point(int level)
{
	if (battle_config.use_statpoint_table) //Use values from "db/statpoint.txt"
		return (pc->statp[level+1] - pc->statp[level]);
	else //Default increase
		return ((level+15) / 5);
}

/// Returns the number of stat points needed to change the specified stat by val.
/// If val is negative, returns the number of stat points that would be needed to
/// raise the specified stat from (current value - val) to current value.
static int pc_need_status_point(struct map_session_data *sd, int type, int val)
{
	int low, high, sp = 0;

	if ( val == 0 )
		return 0;

	low = pc->getstat(sd,type);

	if ( low >= pc_maxparameter(sd) && val > 0 )
		return 0; // Official servers show '0' when max is reached

	high = low + val;

	if ( val < 0 )
		swap(low, high);

	for ( ; low < high; low++ )
#ifdef RENEWAL // renewal status point cost formula
		sp += (low < 100) ? (2 + (low - 1) / 10) : (16 + 4 * ((low - 100) / 5));
#else
		sp += ( 1 + (low + 9) / 10 );
#endif

	return sp;
}

/**
 * Returns the value the specified stat can be increased by with the current
 * amount of available status points for the current character's class.
 *
 * @param sd   The target character.
 * @param type Stat to verify.
 * @return Maximum value the stat could grow by.
 */
static int pc_maxparameterincrease(struct map_session_data *sd, int type)
{
	int base, final, status_points = sd->status.status_point;

	base = final = pc->getstat(sd, type);

	while (final <= pc_maxparameter(sd) && status_points >= 0) {
#ifdef RENEWAL // renewal status point cost formula
		status_points -= (final < 100) ? (2 + (final - 1) / 10) : (16 + 4 * ((final - 100) / 5));
#else
		status_points -= ( 1 + (final + 9) / 10 );
#endif
		final++;
	}
	final--;

	return final > base ? final-base : 0;
}

/**
 * Raises a stat by the specified amount.
 *
 * Obeys max_parameter limits.
 * Subtracts status points according to the cost of the increased stat points.
 *
 * @param sd       The target character.
 * @param type     The stat to change (see enum status_point_types)
 * @param increase The stat increase (strictly positive) amount.
 * @retval true  if the stat was increased by any amount.
 * @retval false if there were no changes.
 */
static bool pc_statusup(struct map_session_data *sd, int type, int increase)
{
	nullpo_ret(sd);
	int realIncrease = increase;

	// check conditions
	if (type < SP_STR || type > SP_LUK || realIncrease <= 0) {
		clif->statusupack(sd, type, 0, increase);
		return false;
	}

	// check limits
	int current = pc->getstat(sd, type);
	int max_increase = pc->maxparameterincrease(sd, type);
	realIncrease = cap_value(realIncrease, 0, max_increase); // cap to the maximum status points available
	if (realIncrease <= 0 || current + realIncrease > pc_maxparameter(sd)) {
		clif->statusupack(sd, type, 0, increase);
		return false;
	}

	// check status points
	int needed_points = pc->need_status_point(sd, type, realIncrease);
	if (needed_points < 0 || needed_points > sd->status.status_point) { // Sanity check
		clif->statusupack(sd, type, 0, increase);
		return false;
	}

	// set new values
	int final_value = pc->setstat(sd, type, current + realIncrease);
	sd->status.status_point -= needed_points;

	status_calc_pc(sd, SCO_NONE);

	// update increase cost indicator
	clif->updatestatus(sd, SP_USTR + type-SP_STR);

	// update statpoint count
	clif->updatestatus(sd, SP_STATUSPOINT);

	// update stat value
	clif->statusupack(sd, type, 1, final_value); // required
	if (final_value > 255)
		clif->updatestatus(sd, type); // send after the 'ack' to override the truncated value

	return true;
}

/**
 * Raises a stat by the specified amount.
 *
 * Obeys max_parameter limits.
 * Does not subtract status points for the cost of the modified stat points.
 *
 * @param sd   The target character.
 * @param type The stat to change (see enum status_point_types)
 * @param val  The stat increase (or decrease) amount.
 * @return the stat increase amount.
 * @retval 0 if no changes were made.
 */
static int pc_statusup2(struct map_session_data *sd, int type, int val)
{
	int max, need;
	nullpo_ret(sd);

	if( type < SP_STR || type > SP_LUK )
	{
		clif->statusupack(sd,type,0,0);
		return 0;
	}

	need = pc->need_status_point(sd,type,1);

	// set new value
	max = pc_maxparameter(sd);
	val = pc->setstat(sd, type, cap_value(pc->getstat(sd,type) + val, 1, max));

	status_calc_pc(sd,SCO_NONE);

	// update increase cost indicator
	if( need != pc->need_status_point(sd,type,1) )
		clif->updatestatus(sd, SP_USTR + type-SP_STR);

	// update stat value
	clif->statusupack(sd,type,1,val); // required
	if( val > 255 )
		clif->updatestatus(sd,type); // send after the 'ack' to override the truncated value

	return val;
}

/*==========================================
 * Update skill_lv for player sd
 * Skill point allocation
 *------------------------------------------*/
static int pc_skillup(struct map_session_data *sd, uint16 skill_id)
{
	int index = 0;
	nullpo_ret(sd);

	if( skill_id >= GD_SKILLBASE && skill_id < GD_SKILLBASE+MAX_GUILDSKILL ) {
		guild->skillup(sd, skill_id);
		return 0;
	}

	if( skill_id >= HM_SKILLBASE && skill_id < HM_SKILLBASE+MAX_HOMUNSKILL && sd->hd ) {
		homun->skillup(sd->hd, skill_id);
		return 0;
	}

	if( !(index = skill->get_index(skill_id)) )
		return 0;

	if( sd->status.skill_point > 0 &&
		sd->status.skill[index].id &&
		sd->status.skill[index].flag == SKILL_FLAG_PERMANENT && //Don't allow raising while you have granted skills. [Skotlex]
		sd->status.skill[index].lv < skill->tree_get_max(skill_id, sd->status.class) )
	{
		sd->status.skill[index].lv++;
		sd->status.skill_point--;
		if( !skill->dbs->db[index].inf )
			status_calc_pc(sd,SCO_NONE); // Only recalculate for passive skills.
		else if (sd->status.skill_point == 0 && (sd->job & MAPID_UPPERMASK) == MAPID_TAEKWON
		      && sd->status.base_level >= 90 && pc->fame_rank(sd->status.char_id, RANKTYPE_TAEKWON) > 0)
			pc->calc_skilltree(sd); // Required to grant all TK Ranger skills.
		else
			pc->check_skilltree(sd, skill_id); // Check if a new skill can Lvlup

		clif->skillup(sd,skill_id, sd->status.skill[index].lv, 1);
		clif->updatestatus(sd,SP_SKILLPOINT);
		if( skill_id == GN_REMODELING_CART ) /* cart weight info was updated by status_calc_pc */
			clif->updatestatus(sd,SP_CARTINFO);
		if (!pc_has_permission(sd, PC_PERM_ALL_SKILL)) // may skill everything at any time anyways, and this would cause a huge slowdown
			clif->skillinfoblock(sd);
	} else if (battle_config.skillup_limit) {
		if (sd->sktree.second != 0) {
#if PACKETVER >= 20090805
			clif->msgtable_num(sd, MSG_UPGRADESKILLERROR_MORE_FIRSTJOBSKILL, sd->sktree.second);
#endif
		} else if (sd->sktree.third != 0) {
#if PACKETVER >= 20091013
			clif->msgtable_num(sd, MSG_UPGRADESKILLERROR_MORE_SECONDJOBSKILL, sd->sktree.third);
#endif
		} else if (pc->calc_skillpoint(sd) < 9) {  /* TODO: official response? */
			clif->messagecolor_self(sd->fd, COLOR_RED, msg_sd(sd, 164)); // "You need to learn the basic skills first."
		}
	}
	return 0;
}

/*==========================================
 * /allskill
 *------------------------------------------*/
static int pc_allskillup(struct map_session_data *sd)
{
	int i;

	nullpo_ret(sd);

	for (i = 0; i < MAX_SKILL_DB; i++) {
		if (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY || sd->status.skill[i].flag >= SKILL_FLAG_REPLACED_LV_0) {
			sd->status.skill[i].lv = (sd->status.skill[i].flag == SKILL_FLAG_TEMPORARY) ? 0 : sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0;
			sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
			if (sd->status.skill[i].lv == 0)
				sd->status.skill[i].id = 0;
		}
	}

	if (pc_has_permission(sd, PC_PERM_ALL_SKILL)) { //Get ALL skills except npc/guild ones. [Skotlex]
		//and except SG_DEVIL [Komurka] and MO_TRIPLEATTACK and RG_SNATCHER [ultramage]
		for (i = 0; i < MAX_SKILL_DB; i++) {
			switch( skill->dbs->db[i].nameid ) {
				case SG_DEVIL:
				case MO_TRIPLEATTACK:
				case RG_SNATCHER:
					continue;
				default:
					if( !(skill->dbs->db[i].inf2&(INF2_NPC_SKILL|INF2_GUILD_SKILL)) )
						if ( ( sd->status.skill[i].lv = skill->dbs->db[i].max ) )//Nonexistant skills should return a max of 0 anyway.
							sd->status.skill[i].id = skill->dbs->db[i].nameid;
			}
		}
	} else {
		int id;
		for (i = 0; i < MAX_SKILL_TREE && (id=pc->skill_tree[pc->class2idx(sd->status.class)][i].id) > 0; i++) {
			int idx = pc->skill_tree[pc->class2idx(sd->status.class)][i].idx;
			int inf2 = skill->dbs->db[idx].inf2;
			if (
				(inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn) ||
				(inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL)) ||
				id==SG_DEVIL
			)
				continue; //Cannot be learned normally.

			sd->status.skill[idx].id = id;
			sd->status.skill[idx].lv = skill->tree_get_max(id, sd->status.class); // celest
		}
	}
	status_calc_pc(sd,SCO_NONE);
	//Required because if you could level up all skills previously,
	//the update will not be sent as only the lv variable changes.
	clif->skillinfoblock(sd);
	return 0;
}

/*==========================================
 * /resetlvl
 *------------------------------------------*/
static int pc_resetlvl(struct map_session_data *sd, int type)
{
	int  i;

	nullpo_ret(sd);

	if (type != 3) //Also reset skills
		pc->resetskill(sd, PCRESETSKILL_NONE);

	if(type == 1) {
		sd->status.skill_point=0;
		sd->status.base_level=1;
		sd->status.job_level=1;
		sd->status.base_exp=0;
		sd->status.job_exp=0;
		if(sd->sc.option !=0)
			sd->sc.option = 0;

		sd->status.str=1;
		sd->status.agi=1;
		sd->status.vit=1;
		sd->status.int_=1;
		sd->status.dex=1;
		sd->status.luk=1;
		if (sd->status.class == JOB_NOVICE_HIGH) {
			sd->status.status_point=100; // not 88 [celest]
			// give platinum skills upon changing
			pc->skill(sd, NV_FIRSTAID, 1, SKILL_GRANT_PERMANENT);
			pc->skill(sd, NV_TRICKDEAD, 1, SKILL_GRANT_PERMANENT);
		}
	}

	if(type == 2){
		sd->status.skill_point=0;
		sd->status.base_level=1;
		sd->status.job_level=1;
		sd->status.base_exp=0;
		sd->status.job_exp=0;
	}
	if(type == 3){
		sd->status.base_level=1;
		sd->status.base_exp=0;
	}
	if(type == 4){
		sd->status.job_level=1;
		sd->status.job_exp=0;
	}

	clif->updatestatus(sd,SP_STATUSPOINT);
	clif->updatestatus(sd,SP_STR);
	clif->updatestatus(sd,SP_AGI);
	clif->updatestatus(sd,SP_VIT);
	clif->updatestatus(sd,SP_INT);
	clif->updatestatus(sd,SP_DEX);
	clif->updatestatus(sd,SP_LUK);
	clif->updatestatus(sd,SP_BASELEVEL);
	clif->updatestatus(sd,SP_JOBLEVEL);
	clif->updatestatus(sd,SP_STATUSPOINT);
	clif->updatestatus(sd,SP_BASEEXP);
	clif->updatestatus(sd,SP_JOBEXP);
	clif->updatestatus(sd,SP_NEXTBASEEXP);
	clif->updatestatus(sd,SP_NEXTJOBEXP);
	clif->updatestatus(sd,SP_SKILLPOINT);

	clif->updatestatus(sd,SP_USTR); // Updates needed stat points - Valaris
	clif->updatestatus(sd,SP_UAGI);
	clif->updatestatus(sd,SP_UVIT);
	clif->updatestatus(sd,SP_UINT);
	clif->updatestatus(sd,SP_UDEX);
	clif->updatestatus(sd,SP_ULUK); // End Addition

	for(i=0;i<EQI_MAX;i++) { // unequip items that can't be equipped by base 1 [Valaris]
		if(sd->equip_index[i] >= 0)
			if(!pc->isequip(sd,sd->equip_index[i]))
				pc->unequipitem(sd, sd->equip_index[i], PCUNEQUIPITEM_FORCE);
	}

	if ((type == 1 || type == 2 || type == 3) && sd->status.party_id)
		party->send_levelup(sd);

	status_calc_pc(sd,SCO_FORCE);
	clif->skillinfoblock(sd);

	return 0;
}
/*==========================================
 * /resetstate
 *------------------------------------------*/
static int pc_resetstate(struct map_session_data *sd)
{
	nullpo_ret(sd);

	if (battle_config.use_statpoint_table) {
		// New statpoint table used here - Dexity
		if (sd->status.base_level > MAX_LEVEL) {
			//pc->statp[] goes out of bounds, can't reset!
			ShowError("pc_resetstate: Can't reset stats of %d:%d, the base level (%d) is greater than the max level supported (%d)\n",
				sd->status.account_id, sd->status.char_id, sd->status.base_level, MAX_LEVEL);
			return 0;
		}

		sd->status.status_point = pc->statp[sd->status.base_level] + ((sd->job & JOBL_UPPER) != 0 ? 52 : 0); // extra 52+48=100 stat points
	}
	else
	{
		int add=0;
		add += pc->need_status_point(sd, SP_STR, 1-pc->getstat(sd, SP_STR));
		add += pc->need_status_point(sd, SP_AGI, 1-pc->getstat(sd, SP_AGI));
		add += pc->need_status_point(sd, SP_VIT, 1-pc->getstat(sd, SP_VIT));
		add += pc->need_status_point(sd, SP_INT, 1-pc->getstat(sd, SP_INT));
		add += pc->need_status_point(sd, SP_DEX, 1-pc->getstat(sd, SP_DEX));
		add += pc->need_status_point(sd, SP_LUK, 1-pc->getstat(sd, SP_LUK));

		sd->status.status_point+=add;
	}

	pc->setstat(sd, SP_STR, 1);
	pc->setstat(sd, SP_AGI, 1);
	pc->setstat(sd, SP_VIT, 1);
	pc->setstat(sd, SP_INT, 1);
	pc->setstat(sd, SP_DEX, 1);
	pc->setstat(sd, SP_LUK, 1);

	clif->updatestatus(sd,SP_STR);
	clif->updatestatus(sd,SP_AGI);
	clif->updatestatus(sd,SP_VIT);
	clif->updatestatus(sd,SP_INT);
	clif->updatestatus(sd,SP_DEX);
	clif->updatestatus(sd,SP_LUK);

	clif->updatestatus(sd,SP_USTR); // Updates needed stat points - Valaris
	clif->updatestatus(sd,SP_UAGI);
	clif->updatestatus(sd,SP_UVIT);
	clif->updatestatus(sd,SP_UINT);
	clif->updatestatus(sd,SP_UDEX);
	clif->updatestatus(sd,SP_ULUK); // End Addition

	clif->updatestatus(sd,SP_STATUSPOINT);

	if( sd->mission_mobid ) { //bugreport:2200
		sd->mission_mobid = 0;
		sd->mission_count = 0;
		pc_setglobalreg(sd,script->add_variable("TK_MISSION_ID"), 0);
	}

	status_calc_pc(sd,SCO_NONE);

	return 1;
}

/*==========================================
 * /resetskill
 * @param flag: @see enum pc_resetskill_flag
 *------------------------------------------*/
static int pc_resetskill(struct map_session_data *sd, int flag)
{
	int i, inf2, skill_point=0;
	nullpo_ret(sd);

	if (flag&PCRESETSKILL_CHSEX && (sd->job & MAPID_UPPERMASK) != MAPID_BARDDANCER)
		return 0;

	if( !(flag&PCRESETSKILL_RECOUNT) ) { //Remove stuff lost when resetting skills.

		/**
		 * It has been confirmed on official server that when you reset skills with a ranked tweakwon your skills are not reset (because you have all of them anyway)
		 **/
		if ((sd->job & MAPID_UPPERMASK) == MAPID_TAEKWON && sd->status.base_level >= 90 && pc->fame_rank(sd->status.char_id, RANKTYPE_TAEKWON))
			return 0;

		if( pc->checkskill(sd, SG_DEVIL) &&  !pc->nextjobexp(sd) ) //Remove perma blindness due to skill-reset. [Skotlex]
			clif->sc_end(&sd->bl, sd->bl.id, SELF, status->get_sc_icon(SC_DEVIL1));
		i = sd->sc.option;
		if( i&OPTION_RIDING && pc->checkskill(sd, KN_RIDING) )
			i &= ~OPTION_RIDING;
		if( i&OPTION_FALCON && pc->checkskill(sd, HT_FALCON) )
			i &= ~OPTION_FALCON;
		if( i&OPTION_DRAGON && pc->checkskill(sd, RK_DRAGONTRAINING) )
			i &= ~OPTION_DRAGON;
		if( i&OPTION_WUG && pc->checkskill(sd, RA_WUGMASTERY) )
			i &= ~OPTION_WUG;
		if( i&OPTION_WUGRIDER && pc->checkskill(sd, RA_WUGRIDER) )
			i &= ~OPTION_WUGRIDER;
		if (i&OPTION_MADOGEAR && (sd->job & MAPID_THIRDMASK) == MAPID_MECHANIC)
			i &= ~OPTION_MADOGEAR;
#ifndef NEW_CARTS
		if( i&OPTION_CART && pc->checkskill(sd, MC_PUSHCART) )
			i &= ~OPTION_CART;
#else
		if( sd->sc.data[SC_PUSH_CART] )
			pc->setcart(sd, 0);
#endif
		if( i != sd->sc.option )
			pc->setoption(sd, i);

		if( homun_alive(sd->hd) && pc->checkskill(sd, AM_CALLHOMUN) )
			homun->vaporize(sd, HOM_ST_REST, true);

		if ((sd->sc.data[SC_SPRITEMABLE] && pc->checkskill(sd, SU_SPRITEMABLE)))
			status_change_end(&sd->bl, SC_SPRITEMABLE, INVALID_TIMER);
	}

	for (i = 1; i < MAX_SKILL_DB; i++) {
		int lv = sd->status.skill[i].lv;
		if (lv < 1) continue;

		inf2 = skill->dbs->db[i].inf2;

		if( inf2&(INF2_WEDDING_SKILL|INF2_SPIRIT_SKILL) ) //Avoid reseting wedding/linker skills.
			continue;

		if (pc->resetskill_job(sd, i))
			continue;

		if( sd->status.skill[i].flag == SKILL_FLAG_PERM_GRANTED )
			continue;

		if( flag&PCRESETSKILL_CHSEX && !skill_ischangesex(i) )
			continue;

		if( inf2&INF2_QUEST_SKILL && !battle_config.quest_skill_learn ) {
			//Only handle quest skills in a special way when you can't learn them manually
			if( battle_config.quest_skill_reset && !(flag&PCRESETSKILL_RECOUNT) ) { //Wipe them
				sd->status.skill[i].lv = 0;
				sd->status.skill[i].flag = 0;
			}
			continue;
		}
		if( sd->status.skill[i].flag == SKILL_FLAG_PERMANENT )
			skill_point += lv;
		else if( sd->status.skill[i].flag >= SKILL_FLAG_REPLACED_LV_0 )
			skill_point += (sd->status.skill[i].flag - SKILL_FLAG_REPLACED_LV_0);

		if( !(flag&PCRESETSKILL_RECOUNT) ) {// reset
			sd->status.skill[i].lv = 0;
			sd->status.skill[i].flag = 0;
		}
	}

	if( flag&PCRESETSKILL_RECOUNT || !skill_point ) return skill_point;

	sd->status.skill_point += skill_point;

	if (!(flag&PCRESETSKILL_RECOUNT)) {
		// Remove all SCs that can't be inactivated without a skill
		if( sd->sc.data[SC_STORMKICK_READY] )
			status_change_end(&sd->bl, SC_STORMKICK_READY, INVALID_TIMER);
		if( sd->sc.data[SC_DOWNKICK_READY] )
			status_change_end(&sd->bl, SC_DOWNKICK_READY, INVALID_TIMER);
		if( sd->sc.data[SC_TURNKICK_READY] )
			status_change_end(&sd->bl, SC_TURNKICK_READY, INVALID_TIMER);
		if( sd->sc.data[SC_COUNTERKICK_READY] )
			status_change_end(&sd->bl, SC_COUNTERKICK_READY, INVALID_TIMER);
		if( sd->sc.data[SC_DODGE_READY] )
			status_change_end(&sd->bl, SC_DODGE_READY, INVALID_TIMER);
	}

	if (flag&PCRESETSKILL_RESYNC) {
		clif->updatestatus(sd,SP_SKILLPOINT);
		clif->skillinfoblock(sd);
		status_calc_pc(sd,SCO_FORCE);
	}

	return skill_point;
}

static bool pc_resetskill_job(struct map_session_data *sd, int index)
{
	uint16 skill_id;

	nullpo_retr(false, sd);
	Assert_retr(false, index >= 0 && index < MAX_SKILL_DB);

	skill_id = skill->dbs->db[index].nameid;

	// Don't reset trick dead if not a novice/baby
	if (skill_id == NV_TRICKDEAD && (sd->job & MAPID_UPPERMASK) != MAPID_NOVICE) {
		sd->status.skill[index].lv = 0;
		sd->status.skill[index].flag = 0;
		return true;
	}

	// do not reset basic skill
	if (skill_id == NV_BASIC && (sd->job & MAPID_UPPERMASK) != MAPID_NOVICE)
		return true;
	if (skill_id == SU_BASIC_SKILL && (sd->job & MAPID_BASEMASK) != MAPID_SUMMONER)
		return true;
	return false;
}

/*==========================================
 * /resetfeel [Komurka]
 *------------------------------------------*/
static int pc_resetfeel(struct map_session_data *sd)
{
	int i;
	nullpo_ret(sd);

	for (i=0; i<MAX_PC_FEELHATE; i++)
	{
		sd->feel_map[i].m = -1;
		sd->feel_map[i].index = 0;
		pc_setglobalreg(sd,script->add_variable(pc->sg_info[i].feel_var),0);
	}

	return 0;
}

static int pc_resethate(struct map_session_data *sd)
{
	int i;
	nullpo_ret(sd);

	for (i = 0; i < MAX_PC_FEELHATE; i++) {
		sd->hate_mob[i] = -1;
		pc_setglobalreg(sd,script->add_variable(pc->sg_info[i].hate_var),0);
	}
	return 0;
}

static int pc_skillatk_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i, bonus = 0;
	nullpo_ret(sd);

	ARR_FIND(0, ARRAYLENGTH(sd->skillatk), i, sd->skillatk[i].id == skill_id);
	if( i < ARRAYLENGTH(sd->skillatk) ) bonus = sd->skillatk[i].val;

	if(sd->sc.data[SC_PYROTECHNIC_OPTION] || sd->sc.data[SC_AQUAPLAY_OPTION])
		bonus += 10;

	return bonus;
}

static int pc_sub_skillatk_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i, bonus = 0;
	nullpo_ret(sd);

	ARR_FIND(0, ARRAYLENGTH(sd->subskill), i, sd->subskill[i].id == skill_id);

	if (i < ARRAYLENGTH(sd->subskill))
		bonus = sd->subskill[i].val;

	return bonus;
}

static int pc_skillheal_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i, bonus = sd->bonus.add_heal_rate;

	if (bonus) {
		switch (skill_id) {
		case AL_HEAL:
			if ((battle_config.skill_add_heal_rate & 1) == 0)
				bonus = 0;
			break;
		case PR_SANCTUARY:
			if ((battle_config.skill_add_heal_rate & 2) == 0)
				bonus = 0;
			break;
		case AM_POTIONPITCHER:
			if ((battle_config.skill_add_heal_rate & 4) == 0)
				bonus = 0;
			break;
		case CR_SLIMPITCHER:
			if ((battle_config.skill_add_heal_rate & 8) == 0)
				bonus = 0;
			break;
		case BA_APPLEIDUN:
			if ((battle_config.skill_add_heal_rate & 16) == 0)
				bonus = 0;
			break;
		case AB_HIGHNESSHEAL:
			if ((battle_config.skill_add_heal_rate & 32) == 0)
				bonus = 0;
			break;
		}
	}

	ARR_FIND(0, ARRAYLENGTH(sd->skillheal), i, sd->skillheal[i].id == skill_id);

	if (i < ARRAYLENGTH(sd->skillheal))
		bonus += sd->skillheal[i].val;

	return bonus;
}

static int pc_skillheal2_bonus(struct map_session_data *sd, uint16 skill_id)
{
	int i, bonus = sd->bonus.add_heal2_rate;

	ARR_FIND(0, ARRAYLENGTH(sd->skillheal2), i, sd->skillheal2[i].id == skill_id);

	if( i < ARRAYLENGTH(sd->skillheal2) )
		bonus += sd->skillheal2[i].val;

	return bonus;
}

static void pc_respawn(struct map_session_data *sd, enum clr_type clrtype)
{
	if( !pc_isdead(sd) )
		return; // not applicable
	if( sd->bg_id && bg->member_respawn(sd) )
		return; // member revived by battleground

	pc->setstand(sd);
	pc->setrestartvalue(sd,3);
	if( pc->setpos(sd, sd->status.save_point.map, sd->status.save_point.x, sd->status.save_point.y, clrtype) )
		clif->resurrection(&sd->bl, 1); //If warping fails, send a normal stand up packet.
}

static int pc_respawn_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);
	if( sd != NULL )
	{
		sd->pvp_point=0;
		pc->respawn(sd,CLR_OUTSIGHT);
	}

	return 0;
}

/*==========================================
 * Invoked when a player has received damage
 *------------------------------------------*/
static void pc_damage(struct map_session_data *sd, struct block_list *src, unsigned int hp, unsigned int sp)
{
	if (sp) clif->updatestatus(sd,SP_SP);
	if (hp) clif->updatestatus(sd,SP_HP);
	else return;

	if( !src || src == &sd->bl )
		return;

	if( pc_issit(sd) ) {
		pc->setstand(sd);
		skill->sit(sd,0);
	}

	if( sd->progressbar.npc_id ){
		clif->progressbar_abort(sd);
		sd->state.workinprogress = 0;
	}

	if( sd->status.pet_id > 0 && sd->pd && battle_config.pet_damage_support )
		pet->target_check(sd,src,1);

	if (sd->status.ele_id != 0 && sd->ed != NULL)
		elemental->set_target(sd,src);

	if (battle_config.prevent_logout_trigger & PLT_DAMAGE)
		sd->canlog_tick = timer->gettick();

	// Achievements [Smokexyz/Hercules]
	if (src != NULL) {
		if (src->type == BL_PC)
			achievement->validate_pc_damage(BL_UCAST(BL_PC, src), sd, hp);
		else if (src->type == BL_MOB)
			achievement->validate_mob_damage(sd, hp, true);
	}
}

/**
 * Invoked when a character died.
 *
 * @param sd The died character.
 * @param src The unit which caused the death.
 * @retval 0 Death canceled.
 * @retval 1 Standard death.
 * @retval 9 Died in PVP/GVG/BG.
 *
 **/
static int pc_dead(struct map_session_data *sd, struct block_list *src)
{
	nullpo_ret(sd);

	for (int i = 0; i < MAX_PC_DEVOTION; i++) {
		if (sd->devotion[i] != 0) {
			struct map_session_data *devsd = map->id2sd(sd->devotion[i]);

			if (devsd != NULL)
				status_change_end(&devsd->bl, SC_DEVOTION, INVALID_TIMER);

			sd->devotion[i] = 0;
		}
	}
	for (int i = 0; i < MAX_STELLAR_MARKS; i++) {
		if (sd->stellar_mark[i] != 0) {
			struct map_session_data *smarksd = map->id2sd(sd->stellar_mark[i]);

			if (smarksd != NULL)
				status_change_end(&smarksd->bl, SC_FLASHKICK, INVALID_TIMER);
			sd->stellar_mark[i] = 0;
		}
	}
	for (int i = 0; i < MAX_UNITED_SOULS; i++) {
		if (sd->united_soul[i]) {
			struct map_session_data *usoulsd = map->id2sd(sd->united_soul[i]);

			if (usoulsd != NULL)
				status_change_end(&usoulsd->bl, SC_SOULUNITY, INVALID_TIMER);
			sd->united_soul[i] = 0;
		}
	}

	if (sd->status.pet_id > 0 && sd->pd != NULL) {
		struct pet_data *pd = sd->pd;

		if (map->list[sd->bl.m].flag.noexppenalty == 0)
			pet->set_intimate(pd, pd->pet.intimate - pd->petDB->die);

		if (sd->pd != NULL && sd->pd->target_id != 0) // Unlock all targets.
			pet->unlocktarget(sd->pd);
	}

	if (sd->status.hom_id > 0 && sd->hd != NULL && battle_config.homunculus_auto_vapor != 0)
		homun->vaporize(sd, HOM_ST_REST, true);

	if (sd->md != NULL)
		mercenary->delete(sd->md, MERC_DELETE_RANAWAY); // Your mercenary soldier ran away.

	if (sd->ed != NULL)
		elemental->delete(sd->ed, 0);

	if (battle_config.duel_autoleave_when_die != 0) { // Leave duel if character died. [LuzZza]
		if (sd->duel_group > 0)
			duel->leave(sd->duel_group, sd);
		if (sd->duel_invite > 0)
			duel->reject(sd->duel_invite, sd);
	}

	if (sd->npc_id != 0 && sd->state.using_megaphone == 0 && sd->st != NULL && sd->st->state != RUN)
		npc->event_dequeue(sd);

	pc_setglobalreg(sd, script->add_variable("PC_DIE_COUNTER"), sd->die_counter + 1);
	pc->setparam(sd, SP_KILLERRID, (src != NULL) ? src->id : 0);

	if (sd->bg_id != 0) { //TODO: Purge when bgqueue is deemed ok.
		struct battleground_data *bgd = bg->team_search(sd->bg_id);

		if (bgd != NULL && bgd->die_event[0] != '\0')
			npc->event(sd, bgd->die_event, 0);
	}

	for (int i = 0; i < VECTOR_LENGTH(sd->script_queues); i++) {
		struct script_queue *queue = script->queue(VECTOR_INDEX(sd->script_queues, i));

		if (queue != NULL && queue->event_death[0] != '\0')
			npc->event(sd, queue->event_death, 0);
	}

	npc->script_event(sd, NPCE_DIE);

	// Clear anything NPC-related if character died while interacting with one.
	if (((sd->npc_id != 0 && sd->state.using_megaphone == 0) || sd->npc_shopid != 0) && sd->state.dialog != 0) {
		if (sd->state.using_fake_npc != 0) {
			clif->clearunit_single(sd->npc_id, CLR_OUTSIGHT, sd->fd);
			sd->state.using_fake_npc = 0;
		}

		if (sd->state.menu_or_input != 0)
			sd->state.menu_or_input = 0;

		if (sd->npc_menu != 0)
			sd->npc_menu = 0;

		sd->npc_id = 0;
		sd->npc_shopid = 0;

		if (sd->st != NULL && sd->st->state != END)
			sd->st->state = END;
	}

	// E.g. not killed through pc->damage().
	if (pc_issit(sd))
		clif->sc_end(&sd->bl, sd->bl.id, SELF, status->get_sc_icon(SC_SIT));

	pc_setdead(sd);
	clif->party_dead_notification(sd);

	pc->autocast_clear(sd); // Unset auto-cast data.

	if (sd->menuskill_id != 0) { // Reset menu skills.
		sd->menuskill_id = 0;
		sd->menuskill_val = 0;
	}

	// Reset ticks.
	sd->hp_loss.tick = 0;
	sd->sp_loss.tick = 0;
	sd->hp_regen.tick = 0;
	sd->sp_regen.tick = 0;

	if (sd->spiritball != 0)
		pc->delspiritball(sd, sd->spiritball, 0);

	if (sd->soulball != 0)
		pc->delsoulball(sd, sd->soulball, false);

	if (sd->charm_type != CHARM_TYPE_NONE && sd->charm_count > 0)
		pc->del_charm(sd, sd->charm_count, sd->charm_type);

	int64 tick = timer->gettick();

	if (src != NULL) {
		switch (src->type) {
		case BL_MOB: {
			struct mob_data *md = BL_UCAST(BL_MOB, src);

			if (md->target_id == sd->bl.id)
				mob->unlocktarget(md, tick);

			if (battle_config.mobs_level_up != 0 && md->status.hp != 0 && md->level < pc->maxbaselv(sd)
			    && md->guardian_data == NULL && md->special_state.ai == AI_NONE) { // Guardians/summons should not level up. [Skotlex]
				/// Monster level up. [Valaris]
				clif->misceffect(&md->bl, 0);
				md->level++;
				status_calc_mob(md, SCO_NONE);
				status_percent_heal(src, 10, 0);

				if ((battle_config.show_mob_info & 4) != 0)
					clif->blname_ack(0, &md->bl); // Update name with new level.
			}

			src = battle->get_master(src); // Maybe character summon.
			break;
		}
		case BL_PET:
			src = &BL_UCAST(BL_PET, src)->msd->bl; // Pass on to master.
			break;
		case BL_HOM:
			src = &BL_UCAST(BL_HOM, src)->master->bl; // Pass on to master.
			break;
		case BL_MER:
			src = &BL_UCAST(BL_MER, src)->master->bl; // Pass on to master.
			break;
		case BL_NUL:
		case BL_PC:
		case BL_ITEM:
		case BL_SKILL:
		case BL_NPC:
		case BL_CHAT:
		case BL_ELEM:
		case BL_ALL:
			break;
		}
	}

	if (src != NULL && src->type == BL_PC) {
		struct map_session_data *ssd = BL_UCAST(BL_PC, src);

		pc->setparam(ssd, SP_KILLEDRID, sd->bl.id);
		npc->script_event(ssd, NPCE_KILLPC);
		achievement->validate_pc_kill(ssd, sd);

		if ((battle_config.pk_mode & 2) != 0) {
			ssd->status.manner -= 5;

			if (ssd->status.manner < 0)
				sc_start(NULL, src, SC_NOCHAT, 100, 0, 0, 0);

#if 0
			// PK/Karma system code (not enabled yet) [celest]
			// originally from Kade Online, so i don't know if any of these is correct ^^;
			// note: karma is measured REVERSE, so more karma = more 'evil' / less honourable,
			// karma going down = more 'good' / more honourable.
			// The Karma System way...

			if (sd->status.karma > ssd->status.karma) {
				// If player killed was more evil
				sd->status.karma--;
				ssd->status.karma--;
			} else if (sd->status.karma < ssd->status.karma) { // If player killed was more good
				ssd->status.karma++;
			}

			// or the PK System way...

			if (sd->status.karma > 0) // player killed is dishonourable?
				ssd->status.karma--; // honour points earned

			sd->status.karma++; // honour points lost

			// To-do: Receive exp on certain occasions
#endif
		}
	}

	if (battle_config.bone_drop == 2 || (battle_config.bone_drop == 1 && map->list[sd->bl.m].flag.pvp != 0)) {
		struct item item_tmp;

		memset(&item_tmp, 0, sizeof(item_tmp));
		item_tmp.nameid = ITEMID_SKULL_;
		item_tmp.identify = 1;
		item_tmp.card[0] = CARD0_CREATE;
		item_tmp.card[1] = 0;
		item_tmp.card[2] = GetWord(sd->status.char_id, 0);
		item_tmp.card[3] = GetWord(sd->status.char_id, 1);
		map->addflooritem(&sd->bl, &item_tmp, 1, sd->bl.m, sd->bl.x, sd->bl.y, 0, 0, 0, 0, false);
	}

	// Activate Steel Body if a Super Novice dies at 99+% EXP. [celest]
	if ((sd->job & MAPID_UPPERMASK) == MAPID_SUPER_NOVICE && sd->state.snovice_dead_flag == 0) {
		uint64 next = pc->nextbaseexp(sd);

		if (next == 0)
			next = pc->thisbaseexp(sd);

		if (get_percentage64(sd->status.base_exp, next) >= 99) {
			sd->state.snovice_dead_flag = 1;
			pc->setstand(sd);
			status_percent_heal(&sd->bl, 100, 100);
			clif->resurrection(&sd->bl, 1);

			if (battle_config.pc_invincible_time != 0)
				pc->setinvincibletimer(sd, battle_config.pc_invincible_time);

			sc_start(NULL, &sd->bl, skill->get_sc_type(MO_STEELBODY), 100, 5, skill->get_time(MO_STEELBODY, 5), MO_STEELBODY);

			if (map_flag_gvg2(sd->bl.m))
				pc->respawn_timer(INVALID_TIMER, timer->gettick(), sd->bl.id, 0);

			return 0;
		}
	}

	if (battle_config.death_penalty_type != 0 && pc->isDeathPenaltyJob(sd->job) && !map_flag_gvg2(sd->bl.m)
	    && map->list[sd->bl.m].flag.noexppenalty == 0 && sd->sc.data[SC_BABY] == NULL
	    && sd->sc.data[SC_CASH_DEATHPENALTY] == NULL && !pc->auto_exp_insurance(sd)) {
		if (battle_config.death_penalty_base > 0) {
			unsigned int base_penalty = 0;
			int rate = battle_config.death_penalty_base;

			switch (battle_config.death_penalty_type) {
			case 1:
				base_penalty = (unsigned int)apply_percentrate64(pc->nextbaseexp(sd), rate, 10000);
				break;
			case 2:
				base_penalty = (unsigned int)apply_percentrate64(sd->status.base_exp, rate, 10000);
				break;
			}

			if (base_penalty != 0) {
				if (battle_config.pk_mode != 0 && src != NULL && src->type == BL_PC)
					base_penalty *= 2;

				if (sd->status.mod_death != 100)
					base_penalty = base_penalty * sd->status.mod_death / 100;

				sd->status.base_exp -= min(sd->status.base_exp, base_penalty);
				clif->updatestatus(sd, SP_BASEEXP);
			}
		}

		if (battle_config.death_penalty_job > 0) {
			unsigned int job_penalty = 0;
			int rate = battle_config.death_penalty_job;

			switch (battle_config.death_penalty_type) {
			case 1:
				job_penalty = (unsigned int)apply_percentrate64(pc->nextjobexp(sd), rate, 10000);
				break;
			case 2:
				job_penalty = (unsigned int)apply_percentrate64(sd->status.job_exp, rate, 10000);
				break;
			}

			if (job_penalty != 0) {
				if (battle_config.pk_mode != 0 && src != NULL && src->type == BL_PC)
					job_penalty *= 2;

				if (sd->status.mod_death != 100)
					job_penalty = job_penalty * sd->status.mod_death / 100;

				sd->status.job_exp -= min(sd->status.job_exp, job_penalty);
				clif->updatestatus(sd, SP_JOBEXP);
			}
		}

		if (battle_config.zeny_penalty > 0 && map->list[sd->bl.m].flag.nozenypenalty == 0) {
			int zeny_penalty = apply_percentrate(sd->status.zeny, battle_config.zeny_penalty, 10000);

			if (zeny_penalty != 0)
				pc->payzeny(sd, zeny_penalty, LOG_TYPE_PICKDROP_PLAYER, NULL);
		}
	}

	if (map->list[sd->bl.m].flag.pvp_nightmaredrop != 0) {
		// Moved this outside so it works when PVP isn't enabled and during pk mode. [Ancyker]
		for (int i = 0; i < map->list[sd->bl.m].drop_list_count; i++) {
			int id = map->list[sd->bl.m].drop_list[i].drop_id;
			int type = map->list[sd->bl.m].drop_list[i].drop_type;
			int per = map->list[sd->bl.m].drop_list[i].drop_per;

			if (id == 0)
				continue;

			if (id == -1) {
				int eq_num = 0;
				int eq_n[MAX_INVENTORY];

				memset(eq_n, 0, sizeof(eq_n));

				for (int j = 0; j < sd->status.inventorySize; j++) {
					bool is_equipped = (sd->status.inventory[j].equip != 0);

					if ((type == 1 && !is_equipped) || (type == 2 && is_equipped) || type == 3) {
						int k;

						ARR_FIND(0, sd->status.inventorySize, k, eq_n[k] <= 0);

						if (k < sd->status.inventorySize)
							eq_n[k] = j;

						eq_num++;
					}
				}

				if (eq_num > 0) {
					int n = eq_n[rnd() % eq_num];

					if (rnd() % 10000 < per) {
						if (sd->status.inventory[n].equip != 0)
							pc->unequipitem(sd, n, PCUNEQUIPITEM_RECALC|PCUNEQUIPITEM_FORCE);

						pc->dropitem(sd, n, 1);
					}
				}
			} else if (id > 0) {
				for (int j = 0; j < sd->status.inventorySize; j++) {
					bool is_equipped = (sd->status.inventory[j].equip != 0);

					if (((type == 1 && !is_equipped) || (type == 2 && is_equipped) || type == 3)
					    && sd->status.inventory[j].nameid == id && rnd() % 10000 < per) {
						if (is_equipped)
							pc->unequipitem(sd, j, PCUNEQUIPITEM_RECALC|PCUNEQUIPITEM_FORCE);

						pc->dropitem(sd, j, 1);
						break;
					}
				}
			}
		}
	}

	// Remove autotrade to prevent autotrading from save point.
	if ((map->list[sd->bl.m].flag.pvp != 0 || map->list[sd->bl.m].flag.gvg != 0)
	    && (sd->state.standalone != 0 || sd->state.autotrade != 0)) {
		sd->state.autotrade = 0;
		sd->state.standalone = 0;
		pc->autotrade_update(sd, PAUC_REMOVE);
		map->quit(sd);
	}

	// Disable certain PVP functions on pk_mode. [Valaris]
	if (map->list[sd->bl.m].flag.pvp != 0 && battle_config.pk_mode == 0
	    && map->list[sd->bl.m].flag.pvp_nocalcrank == 0) {
		sd->pvp_point -= 5;
		sd->pvp_lost++;

		if (src != NULL && src->type == BL_PC) {
			struct map_session_data *ssd = BL_UCAST(BL_PC, src);

			ssd->pvp_point++;
			ssd->pvp_won++;
		}

		if (sd->pvp_point < 0) {
			timer->add(tick + 1, pc->respawn_timer, sd->bl.id, 0);
			return 1|8;
		}
	}

	// GVG
	if (map_flag_gvg2(sd->bl.m)) {
		timer->add(tick + 1, pc->respawn_timer, sd->bl.id, 0);
		return 1|8;
	}

	if (sd->bg_id != 0) {
		struct battleground_data *bgd = bg->team_search(sd->bg_id);

		if (bgd != NULL && bgd->mapindex > 0) { // Respawn by BG.
			timer->add(tick + 1000, pc->respawn_timer, sd->bl.id, 0);
			return 1|8;
		}
	}

	// Reset "can log out" tick.
	if (battle_config.prevent_logout != 0)
		sd->canlog_tick = timer->gettick() - battle_config.prevent_logout;

	return 1;
}

static bool pc_isDeathPenaltyJob(uint16 job)
{
    return (job & MAPID_UPPERMASK) != MAPID_NOVICE;  // only novices will receive no penalty
}

static void pc_revive(struct map_session_data *sd, unsigned int hp, unsigned int sp)
{
	nullpo_retv(sd);
	if(hp) clif->updatestatus(sd,SP_HP);
	if(sp) clif->updatestatus(sd,SP_SP);

	pc->setstand(sd);
	if(battle_config.pc_invincible_time > 0)
		pc->setinvincibletimer(sd, battle_config.pc_invincible_time);

	if( sd->state.gmaster_flag ) {
		guild->aura_refresh(sd,GD_LEADERSHIP,guild->checkskill(sd->guild,GD_LEADERSHIP));
		guild->aura_refresh(sd,GD_GLORYWOUNDS,guild->checkskill(sd->guild,GD_GLORYWOUNDS));
		guild->aura_refresh(sd,GD_SOULCOLD,guild->checkskill(sd->guild,GD_SOULCOLD));
		guild->aura_refresh(sd,GD_HAWKEYES,guild->checkskill(sd->guild,GD_HAWKEYES));
	}
}
// script
//
/*==========================================
 * script reading pc status registry
 *------------------------------------------*/
static int64 pc_readparam(const struct map_session_data *sd, int type)
{
	int64 val = 0;

	nullpo_ret(sd);

	switch(type) {
		case SP_SKILLPOINT:      val = sd->status.skill_point; break;
		case SP_STATUSPOINT:     val = sd->status.status_point; break;
		case SP_ZENY:            val = sd->status.zeny; break;
		case SP_BANKVAULT:       val = sd->status.bank_vault; break;
		case SP_BASELEVEL:       val = sd->status.base_level; break;
		case SP_JOBLEVEL:        val = sd->status.job_level; break;
		case SP_CLASS:           val = sd->status.class; break;
		case SP_BASEJOB:         val = pc->mapid2jobid(sd->job & MAPID_UPPERMASK, sd->status.sex); break; //Base job, extracting upper type.
		case SP_UPPER:           val = (sd->job & JOBL_UPPER) != 0 ? 1 : ((sd->job & JOBL_BABY) != 0 ? 2 : 0); break;
		case SP_BASECLASS:       val = pc->mapid2jobid(sd->job & MAPID_BASEMASK, sd->status.sex); break; //Extract base class tree. [Skotlex]
		case SP_SEX:             val = sd->status.sex; break;
		case SP_WEIGHT:          val = sd->weight; break;
		case SP_MAXWEIGHT:       val = sd->max_weight; break;
		case SP_BASEEXP:         val = sd->status.base_exp; break;
		case SP_JOBEXP:          val = sd->status.job_exp; break;
		case SP_NEXTBASEEXP:     val = pc->nextbaseexp(sd); break;
		case SP_NEXTJOBEXP:      val = pc->nextjobexp(sd); break;
		case SP_HP:              val = sd->battle_status.hp; break;
		case SP_MAXHP:           val = sd->battle_status.max_hp; break;
		case SP_SP:              val = sd->battle_status.sp; break;
		case SP_MAXSP:           val = sd->battle_status.max_sp; break;
		case SP_STR:             val = sd->status.str; break;
		case SP_AGI:             val = sd->status.agi; break;
		case SP_VIT:             val = sd->status.vit; break;
		case SP_INT:             val = sd->status.int_; break;
		case SP_DEX:             val = sd->status.dex; break;
		case SP_LUK:             val = sd->status.luk; break;
		case SP_KARMA:           val = sd->status.karma; break;
		case SP_MANNER:          val = sd->status.manner; break;
		case SP_FAME:            val = sd->status.fame; break;
		case SP_KILLERRID:       val = sd->killerrid; break;
		case SP_KILLEDRID:       val = sd->killedrid; break;
		case SP_SLOTCHANGE:      val = sd->status.slotchange; break;
		case SP_CHARRENAME:      val = sd->status.rename; break;
		case SP_MOD_EXP:         val = sd->status.mod_exp; break;
		case SP_MOD_DROP:        val = sd->status.mod_drop; break;
		case SP_MOD_DEATH:       val = sd->status.mod_death; break;
		case SP_CRITICAL:        val = sd->battle_status.cri/10; break;
		case SP_ASPD:            val = (2000-sd->battle_status.amotion)/10; break;
		case SP_BASE_ATK:        val = sd->battle_status.batk; break;
		case SP_DEF1:            val = sd->battle_status.def; break;
		case SP_DEF2:            val = sd->battle_status.def2; break;
		case SP_MDEF1:           val = sd->battle_status.mdef; break;
		case SP_MDEF2:           val = sd->battle_status.mdef2; break;
		case SP_HIT:             val = sd->battle_status.hit; break;
		case SP_FLEE1:           val = sd->battle_status.flee; break;
		case SP_FLEE2:           val = sd->battle_status.flee2; break;
		case SP_DEFELE:          val = sd->battle_status.def_ele; break;
		case SP_VARCASTRATE:
#ifdef RENEWAL_CAST
			val = sd->bonus.varcastrate;
			break;
#else
			FALLTHROUGH
#endif
		case SP_CASTRATE:
				val = sd->castrate;
			break;
		case SP_MAXHPRATE:       val = sd->hprate; break;
		case SP_MAXSPRATE:       val = sd->sprate; break;
		case SP_SPRATE:          val = sd->dsprate; break;
		case SP_SPEED_RATE:      val = sd->bonus.speed_rate; break;
		case SP_SPEED_ADDRATE:   val = sd->bonus.speed_add_rate; break;
		case SP_ASPD_RATE:
#ifndef RENEWAL_ASPD
			val = sd->battle_status.aspd_rate;
#else
			val = sd->battle_status.aspd_rate2;
#endif
			break;
		case SP_HP_RECOV_RATE:   val = sd->hprecov_rate; break;
		case SP_SP_RECOV_RATE:   val = sd->sprecov_rate; break;
		case SP_CRITICAL_DEF:    val = sd->bonus.critical_def; break;
		case SP_NEAR_ATK_DEF:    val = sd->bonus.near_attack_def_rate; break;
		case SP_LONG_ATK_DEF:    val = sd->bonus.long_attack_def_rate; break;
		case SP_DOUBLE_RATE:     val = sd->bonus.double_rate; break;
		case SP_DOUBLE_ADD_RATE: val = sd->bonus.double_add_rate; break;
		case SP_MATK_RATE:       val = sd->matk_rate; break;
		case SP_ATK_RATE:        val = sd->bonus.atk_rate; break;
		case SP_MAGIC_ATK_DEF:   val = sd->bonus.magic_def_rate; break;
		case SP_MISC_ATK_DEF:    val = sd->bonus.misc_def_rate; break;
		case SP_PERFECT_HIT_RATE:val = sd->bonus.perfect_hit; break;
		case SP_PERFECT_HIT_ADD_RATE: val = sd->bonus.perfect_hit_add; break;
		case SP_CRITICAL_RATE:   val = sd->critical_rate; break;
		case SP_HIT_RATE:        val = sd->hit_rate; break;
		case SP_FLEE_RATE:       val = sd->flee_rate; break;
		case SP_FLEE2_RATE:      val = sd->flee2_rate; break;
		case SP_DEF_RATE:        val = sd->def_rate; break;
		case SP_DEF2_RATE:       val = sd->def2_rate; break;
		case SP_MDEF_RATE:       val = sd->mdef_rate; break;
		case SP_MDEF2_RATE:      val = sd->mdef2_rate; break;
		case SP_RESTART_FULL_RECOVER: val = sd->special_state.restart_full_recover?1:0; break;
		case SP_NO_CASTCANCEL:   val = sd->special_state.no_castcancel?1:0; break;
		case SP_NO_CASTCANCEL2:  val = sd->special_state.no_castcancel2?1:0; break;
		case SP_NO_SIZEFIX:      val = sd->special_state.no_sizefix?1:0; break;
		case SP_NO_MAGIC_DAMAGE: val = sd->special_state.no_magic_damage; break;
		case SP_NO_WEAPON_DAMAGE:val = sd->special_state.no_weapon_damage; break;
		case SP_NO_MISC_DAMAGE:  val = sd->special_state.no_misc_damage; break;
		case SP_NO_GEMSTONE:     val = sd->special_state.no_gemstone?1:0; break;
		case SP_INTRAVISION:     val = sd->special_state.intravision?1:0; break;
		case SP_NO_KNOCKBACK:    val = sd->special_state.no_knockback?1:0; break;
		case SP_SPLASH_RANGE:    val = sd->bonus.splash_range; break;
		case SP_SPLASH_ADD_RANGE:val = sd->bonus.splash_add_range; break;
		case SP_SHORT_WEAPON_DAMAGE_RETURN: val = sd->bonus.short_weapon_damage_return; break;
		case SP_LONG_WEAPON_DAMAGE_RETURN: val = sd->bonus.long_weapon_damage_return; break;
		case SP_MAGIC_DAMAGE_RETURN: val = sd->bonus.magic_damage_return; break;
		case SP_PERFECT_HIDE:    val = sd->special_state.perfect_hiding?1:0; break;
		case SP_UNBREAKABLE:     val = sd->bonus.unbreakable; break;
		case SP_UNBREAKABLE_WEAPON: val = (sd->bonus.unbreakable_equip&EQP_WEAPON)?1:0; break;
		case SP_UNBREAKABLE_ARMOR: val = (sd->bonus.unbreakable_equip&EQP_ARMOR)?1:0; break;
		case SP_UNBREAKABLE_HELM: val = (sd->bonus.unbreakable_equip&EQP_HELM)?1:0; break;
		case SP_UNBREAKABLE_SHIELD: val = (sd->bonus.unbreakable_equip&EQP_SHIELD)?1:0; break;
		case SP_UNBREAKABLE_GARMENT: val = (sd->bonus.unbreakable_equip&EQP_GARMENT)?1:0; break;
		case SP_UNBREAKABLE_SHOES: val = (sd->bonus.unbreakable_equip&EQP_SHOES)?1:0; break;
		case SP_CLASSCHANGE:     val = sd->bonus.classchange; break;
		case SP_LONG_ATK_RATE:   val = sd->bonus.long_attack_atk_rate; break;
		case SP_BREAK_WEAPON_RATE: val = sd->bonus.break_weapon_rate; break;
		case SP_BREAK_ARMOR_RATE: val = sd->bonus.break_armor_rate; break;
		case SP_ADD_STEAL_RATE:  val = sd->bonus.add_steal_rate; break;
		case SP_DELAYRATE:       val = sd->delayrate; break;
		case SP_CRIT_ATK_RATE:   val = sd->bonus.crit_atk_rate; break;
		case SP_UNSTRIPABLE_WEAPON: val = (sd->bonus.unstripable_equip&EQP_WEAPON)?1:0; break;
		case SP_UNSTRIPABLE:
		case SP_UNSTRIPABLE_ARMOR:
			val = (sd->bonus.unstripable_equip&EQP_ARMOR)?1:0;
			break;
		case SP_UNSTRIPABLE_HELM: val = (sd->bonus.unstripable_equip&EQP_HELM)?1:0; break;
		case SP_UNSTRIPABLE_SHIELD: val = (sd->bonus.unstripable_equip&EQP_SHIELD)?1:0; break;
		case SP_SP_GAIN_VALUE:   val = sd->bonus.sp_gain_value; break;
		case SP_HP_GAIN_VALUE:   val = sd->bonus.hp_gain_value; break;
		case SP_MAGIC_SP_GAIN_VALUE: val = sd->bonus.magic_sp_gain_value; break;
		case SP_MAGIC_HP_GAIN_VALUE: val = sd->bonus.magic_hp_gain_value; break;
		case SP_ADD_HEAL_RATE:   val = sd->bonus.add_heal_rate; break;
		case SP_ADD_HEAL2_RATE:  val = sd->bonus.add_heal2_rate; break;
		case SP_ADD_ITEM_HEAL_RATE: val = sd->bonus.itemhealrate2; break;
		case SP_EMATK:           val = sd->bonus.ematk; break;
		case SP_FIXCASTRATE:     val = sd->bonus.fixcastrate; break;
		case SP_ADD_FIXEDCAST:   val = sd->bonus.add_fixcast; break;
#ifdef RENEWAL_CAST
		case SP_ADD_VARIABLECAST:val = sd->bonus.add_varcast; break;
#endif
	}

	return val;
}

/*==========================================
 * script set pc status registry
 *------------------------------------------*/
static int pc_setparam(struct map_session_data *sd, int type, int64 val)
{
	int delta;
	nullpo_ret(sd);

	switch(type){
	case SP_BASELEVEL:
		if (val > pc->maxbaselv(sd)) //Capping to max
			val = pc->maxbaselv(sd);
		if (val > sd->status.base_level) {
			int stat = 0, i;
			for (i = 0; i < val - sd->status.base_level; i++)
				stat += pc->gets_status_point(sd->status.base_level + i);
			sd->status.status_point += stat;
		}
		sd->status.base_level = (int32)val;
		sd->status.base_exp = 0;
		// clif->updatestatus(sd, SP_BASELEVEL);  // Gets updated at the bottom
		clif->updatestatus(sd, SP_NEXTBASEEXP);
		clif->updatestatus(sd, SP_STATUSPOINT);
		clif->updatestatus(sd, SP_BASEEXP);
		status_calc_pc(sd, SCO_FORCE);
		if(sd->status.party_id)
		{
			party->send_levelup(sd);
		}
		break;
	case SP_JOBLEVEL:
		if (val >= sd->status.job_level) {
			if (val > pc->maxjoblv(sd))
				val = pc->maxjoblv(sd);
			sd->status.skill_point += (int)val - sd->status.job_level;
			clif->updatestatus(sd, SP_SKILLPOINT);
		}
		sd->status.job_level = (int32)val;
		sd->status.job_exp = 0;
		// clif->updatestatus(sd, SP_JOBLEVEL);  // Gets updated at the bottom
		clif->updatestatus(sd, SP_NEXTJOBEXP);
		clif->updatestatus(sd, SP_JOBEXP);
		status_calc_pc(sd, SCO_FORCE);
		break;
	case SP_SKILLPOINT:
		sd->status.skill_point = (int32)val;
		break;
	case SP_STATUSPOINT:
		sd->status.status_point = (int32)val;
		break;
	case SP_ZENY:
		if( val < 0 )
			return 0;// can't set negative zeny
		logs->zeny(sd, LOG_TYPE_SCRIPT, sd, -(sd->status.zeny - cap_value((int32)val, 0, MAX_ZENY)));
		sd->status.zeny = cap_value((int32)val, 0, MAX_ZENY);
		break;
	case SP_BANKVAULT:
		val = cap_value(val, 0, MAX_BANK_ZENY);
		delta = ((int32)val - sd->status.bank_vault);
		sd->status.bank_vault = (int32)val;
		if (map->save_settings & 256) {
			chrif->save(sd, 0); // send to char server
		}
		if (delta > 0) {
			clif->bank_deposit(sd, BDA_SUCCESS);
		} else if (delta < 0) {
			clif->bank_withdraw(sd, BWA_SUCCESS);
		}
		return 1; // the vault uses a different packet
	case SP_BASEEXP:
		if(pc->nextbaseexp(sd) > 0) {
			sd->status.base_exp = val;
			pc->checkbaselevelup(sd);
		}
		break;
	case SP_JOBEXP:
		if(pc->nextjobexp(sd) > 0) {
			sd->status.job_exp = val;
			pc->checkjoblevelup(sd);
		}
		break;
	case SP_SEX:
		sd->status.sex = val ? SEX_MALE : SEX_FEMALE;
		break;
	case SP_WEIGHT:
		sd->weight = (int32)val;
		break;
	case SP_MAXWEIGHT:
		sd->max_weight = (int32)val;
		break;
	case SP_HP:
		sd->battle_status.hp = cap_value((int32)val, 1, (int)sd->battle_status.max_hp);
		break;
	case SP_MAXHP:
		sd->battle_status.max_hp = cap_value((int32)val, 1, battle_config.max_hp);

		if( sd->battle_status.max_hp < sd->battle_status.hp )
		{
			sd->battle_status.hp = sd->battle_status.max_hp;
			clif->updatestatus(sd, SP_HP);
		}
		break;
	case SP_SP:
		sd->battle_status.sp = cap_value((int32)val, 0, (int)sd->battle_status.max_sp);
		break;
	case SP_MAXSP:
		sd->battle_status.max_sp = cap_value((int32)val, 1, battle_config.max_sp);

		if( sd->battle_status.max_sp < sd->battle_status.sp )
		{
			sd->battle_status.sp = sd->battle_status.max_sp;
			clif->updatestatus(sd, SP_SP);
		}
		break;
	case SP_STR:
		sd->status.str = cap_value((int)val, 1, pc_maxparameter(sd));
		break;
	case SP_AGI:
		sd->status.agi = cap_value((int)val, 1, pc_maxparameter(sd));
		break;
	case SP_VIT:
		sd->status.vit = cap_value((int)val, 1, pc_maxparameter(sd));
		break;
	case SP_INT:
		sd->status.int_ = cap_value((int)val, 1, pc_maxparameter(sd));
		break;
	case SP_DEX:
		sd->status.dex = cap_value((int)val, 1, pc_maxparameter(sd));
		break;
	case SP_LUK:
		sd->status.luk = cap_value((int)val, 1, pc_maxparameter(sd));
		break;
	case SP_KARMA:
		sd->status.karma = (int)val;
		break;
	case SP_MANNER:
		sd->status.manner = (int)val;
		if( val < 0 )
			sc_start(NULL, &sd->bl, SC_NOCHAT, 100, 0, 0, 0);
		else {
			status_change_end(&sd->bl, SC_NOCHAT, INVALID_TIMER);
			clif->manner_message(sd, 5);
		}
		return 1; // status_change_start/status_change_end already sends packets warning the client
	case SP_FAME:
		sd->status.fame = (int32)val;
		break;
	case SP_KILLERRID:
		sd->killerrid = (int32)val;
		return 1;
	case SP_KILLEDRID:
		sd->killedrid = (int32)val;
		return 1;
	case SP_SLOTCHANGE:
		sd->status.slotchange = (int32)val;
		return 1;
	case SP_CHARRENAME:
		sd->status.rename = (int32)val;
		return 1;
	case SP_MOD_EXP:
		sd->status.mod_exp = (int32)val;
		return 1;
	case SP_MOD_DROP:
		sd->status.mod_drop = (int32)val;
		return 1;
	case SP_MOD_DEATH:
		sd->status.mod_death = (int32)val;
		return 1;
	default:
		ShowError("pc_setparam: Attempted to set unknown parameter '%d'.\n", type);
		return 0;
	}
	clif->updatestatus(sd,type);

	return 1;
}

/*==========================================
 * HP/SP Healing. If flag is passed, the heal type is through clif->heal, otherwise update status.
 *------------------------------------------*/
static void pc_heal(struct map_session_data *sd, unsigned int hp, unsigned int sp, int type)
{
	nullpo_retv(sd);
	if (type) {
		if (hp)
			clif->heal(sd->fd,SP_HP,hp);
		if (sp)
			clif->heal(sd->fd,SP_SP,sp);
	} else {
		if(hp)
			clif->updatestatus(sd,SP_HP);
		if(sp)
			clif->updatestatus(sd,SP_SP);
	}
	return;
}

/*==========================================
 * HP/SP Recovery
 * Heal player hp and/or sp linearly.
 * Calculate bonus by status.
 *------------------------------------------*/
static int pc_itemheal(struct map_session_data *sd, int itemid, int hp, int sp)
{
	int bonus, tmp;

	nullpo_ret(sd);
	if(hp) {
		int i;
		bonus = 100 + (sd->battle_status.vit<<1)
			+ pc->checkskill(sd,SM_RECOVERY)*10
			+ pc->checkskill(sd,AM_LEARNINGPOTION)*5;
		// A potion produced by an Alchemist in the Fame Top 10 gets +50% effect [DracoRPG]
		if (script->potion_flag > 1)
			bonus += bonus*(script->potion_flag-1)*50/100;
		//All item bonuses.
		bonus += sd->bonus.itemhealrate2;
		//Individual item bonuses.
		for(i = 0; i < ARRAYLENGTH(sd->itemhealrate) && sd->itemhealrate[i].nameid; i++) {
			struct item_data *it = itemdb->exists(sd->itemhealrate[i].nameid);
			if (sd->itemhealrate[i].nameid == itemid || (it && it->group && itemdb->in_group(it->group,itemid))) {
				bonus += bonus*sd->itemhealrate[i].rate/100;
				break;
			}
		}

		tmp = hp*bonus/100;
		if(bonus != 100 && tmp > hp)
			hp = tmp;

		// Recovery Potion
		if( sd->sc.data[SC_HEALPLUS] )
			hp += (int)(hp * sd->sc.data[SC_HEALPLUS]->val1/100.);

		// 2014 Halloween Event : Pumpkin Bonus
		if ( sd->sc.data[SC_MTF_PUMPKIN] && itemid == ITEMID_PUMPKIN )
			hp += (int)(hp * sd->sc.data[SC_MTF_PUMPKIN]->val1/100);

		// Activation Potion
		if (sd->sc.data[SC_VITALIZE_POTION] != NULL)
			hp += hp * sd->sc.data[SC_VITALIZE_POTION]->val3 / 100;
	}
	if(sp) {
		bonus = 100 + (sd->battle_status.int_<<1)
			+ pc->checkskill(sd,MG_SRECOVERY)*10
			+ pc->checkskill(sd,AM_LEARNINGPOTION)*5;
		if (script->potion_flag > 1)
			bonus += bonus*(script->potion_flag-1)*50/100;

		tmp = sp*bonus/100;
		if(bonus != 100 && tmp > sp)
			sp = tmp;
	}
	if( sd->sc.count ) {
		if ( sd->sc.data[SC_CRITICALWOUND] ) {
			hp -= hp * sd->sc.data[SC_CRITICALWOUND]->val2 / 100;
			sp -= sp * sd->sc.data[SC_CRITICALWOUND]->val2 / 100;
		}

		if( sd->sc.data[SC_VITALITYACTIVATION] ){
			hp += hp / 2; // 1.5 times
			sp -= sp / 2;
		}

		if ( sd->sc.data[SC_DEATHHURT] ) {
			hp -= hp * 20 / 100;
			sp -= sp * 20 / 100;
		}

		if( sd->sc.data[SC_WATER_INSIGNIA] && sd->sc.data[SC_WATER_INSIGNIA]->val1 == 2 ) {
			hp += hp / 10;
			sp += sp / 10;
		}
#ifdef RENEWAL
		if( sd->sc.data[SC_EXTREMITYFIST2] )
			sp = 0;
#endif
		if (sd->sc.data[SC_BITESCAR]) {
			hp = 0;
		}

		if (sd->sc.data[SC_NO_RECOVER_STATE]) {
			hp = 0;
			sp = 0;
		}
	}

	return status->heal(&sd->bl, hp, sp, STATUS_HEAL_FORCED);
}

/*==========================================
 * HP/SP Recovery
 * Heal player hp nad/or sp by rate
 *------------------------------------------*/
static int pc_percentheal(struct map_session_data *sd, int hp, int sp)
{
	nullpo_ret(sd);

	if(hp > 100) hp = 100;
	else
	if(hp <-100) hp =-100;

	if(sp > 100) sp = 100;
	else
	if(sp <-100) sp =-100;

	if(hp >= 0 && sp >= 0) //Heal
		return status_percent_heal(&sd->bl, hp, sp);

	if(hp <= 0 && sp <= 0) //Damage (negative rates indicate % of max rather than current), and only kill target IF the specified amount is 100%
		return status_percent_damage(NULL, &sd->bl, hp, sp, hp==-100);

	//Crossed signs
	if(hp) {
		if(hp > 0)
			status_percent_heal(&sd->bl, hp, 0);
		else
			status_percent_damage(NULL, &sd->bl, hp, 0, hp==-100);
	}

	if(sp) {
		if(sp > 0)
			status_percent_heal(&sd->bl, 0, sp);
		else
			status_percent_damage(NULL, &sd->bl, 0, sp, false);
	}
	return 0;
}

static int jobchange_killclone(struct block_list *bl, va_list ap)
{
	struct mob_data *md = NULL;
	int flag = va_arg(ap, int);

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_MOB);
	md = BL_UCAST(BL_MOB, bl);

	if (md->master_id && md->special_state.clone && md->master_id == flag)
		status_kill(&md->bl);
	return 1;
}

/*==========================================
 * Called when player changes job
 * Rewrote to make it tidider [Celest]
 *------------------------------------------*/
static int pc_jobchange(struct map_session_data *sd, int class, int upper)
{
	int i, fame_flag=0;
	int job, idx = 0;

	nullpo_ret(sd);

	if (class < 0)
		return 1;

	//Normalize job.
	job = pc->jobid2mapid(class);
	if (job == -1)
		return 1;
	switch (upper) {
		case 1:
			job |= JOBL_UPPER;
			break;
		case 2:
			job |= JOBL_BABY;
			break;
	}
	//This will automatically adjust bard/dancer classes to the correct gender
	//That is, if you try to jobchange into dancer, it will turn you to bard.
	class = pc->mapid2jobid(job, sd->status.sex);
	if (class == -1)
		return 1;

	if ((uint16)job == sd->job)
		return 1; //Nothing to change.

	if ((job & JOBL_2) != 0 && (sd->job & JOBL_2) == 0 && (job & MAPID_UPPERMASK) != MAPID_SUPER_NOVICE) {
		// changing from 1st to 2nd job
		sd->change_level_2nd = sd->status.job_level;
		pc_setglobalreg(sd, script->add_variable("jobchange_level"), sd->change_level_2nd);
	} else if((job & JOBL_THIRD) != 0 && (sd->job & JOBL_THIRD) == 0) {
		// changing from 2nd to 3rd job
		sd->change_level_3rd = sd->status.job_level;
		pc_setglobalreg(sd, script->add_variable("jobchange_level_3rd"), sd->change_level_3rd);
	}

	if(sd->cloneskill_id) {
		idx = skill->get_index(sd->cloneskill_id);
		if( sd->status.skill[idx].flag == SKILL_FLAG_PLAGIARIZED ) {
			sd->status.skill[idx].id = 0;
			sd->status.skill[idx].lv = 0;
			sd->status.skill[idx].flag = 0;
			clif->deleteskill(sd,sd->cloneskill_id);
		}
		sd->cloneskill_id = 0;
		pc_setglobalreg(sd, script->add_variable("CLONE_SKILL"), 0);
		pc_setglobalreg(sd, script->add_variable("CLONE_SKILL_LV"), 0);
	}

	if(sd->reproduceskill_id) {
		idx = skill->get_index(sd->reproduceskill_id);
		if( sd->status.skill[idx].flag == SKILL_FLAG_PLAGIARIZED ) {
			sd->status.skill[idx].id = 0;
			sd->status.skill[idx].lv = 0;
			sd->status.skill[idx].flag = 0;
			clif->deleteskill(sd,sd->reproduceskill_id);
		}
		sd->reproduceskill_id = 0;
		pc_setglobalreg(sd, script->add_variable("REPRODUCE_SKILL"),0);
		pc_setglobalreg(sd, script->add_variable("REPRODUCE_SKILL_LV"),0);
	}

	if ((job & MAPID_UPPERMASK) != (sd->job & MAPID_UPPERMASK)) { //Things to remove when changing class tree.
		const int class_idx = pc->class2idx(sd->status.class);
		short id;
		for (i = 0; i < MAX_SKILL_TREE && (id = pc->skill_tree[class_idx][i].id) > 0; i++) {
			//Remove status specific to your current tree skills.
			enum sc_type sc = skill->get_sc_type(id);
			if (sc > SC_COMMON_MAX && sd->sc.data[sc])
				status_change_end(&sd->bl, sc, INVALID_TIMER);
		}
	}

	if ((sd->job & MAPID_UPPERMASK) == MAPID_STAR_GLADIATOR && (job & MAPID_UPPERMASK) != MAPID_STAR_GLADIATOR) {
		/* going off star glad lineage, reset feel to not store no-longer-used vars in the database */
		pc->resetfeel(sd);
	}

	sd->status.class = class;
	{
		int fame_list_type = pc->famelist_type(sd->job);
		if (fame_list_type != RANKTYPE_UNKNOWN)
			fame_flag = pc->fame_rank(sd->status.char_id, fame_list_type);
	}
	sd->job = (uint16)job;
	sd->status.job_level=1;
	sd->status.job_exp=0;

	if (sd->status.base_level > pc->maxbaselv(sd)) {
		sd->status.base_level = pc->maxbaselv(sd);
		sd->status.base_exp=0;
		pc->resetstate(sd);
		clif->updatestatus(sd,SP_STATUSPOINT);
		clif->updatestatus(sd,SP_BASELEVEL);
		clif->updatestatus(sd,SP_BASEEXP);
		clif->updatestatus(sd,SP_NEXTBASEEXP);
	}

	clif->updatestatus(sd,SP_JOBLEVEL);
	clif->updatestatus(sd,SP_JOBEXP);
	clif->updatestatus(sd,SP_NEXTJOBEXP);

	for(i=0;i<EQI_MAX;i++) {
		if(sd->equip_index[i] >= 0)
			if(!pc->isequip(sd,sd->equip_index[i]))
				pc->unequipitem(sd,sd->equip_index[i], PCUNEQUIPITEM_FORCE); // unequip invalid item for class
	}

	//Change look, if disguised, you need to undisguise
	//to correctly calculate new job sprite without
	if (sd->disguise != -1)
		pc->disguise(sd, -1);

	// Fix atcommand @jobchange when the player changing from 3rd job having alternate body style into non-3rd job, crashing the client
	if (pc->has_second_costume(sd) == false) {
		sd->status.body = 0;
		sd->vd.body_style = 0;
		clif->changelook(&sd->bl, LOOK_BODY2, sd->vd.body_style);
	}

	status->set_viewdata(&sd->bl, class);
	clif->changelook(&sd->bl, LOOK_BASE, sd->vd.class); // move sprite update to prevent client crashes with incompatible equipment [Valaris]
	if(sd->vd.cloth_color)
		clif->changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
	if (sd->vd.body_style)
		clif->changelook(&sd->bl,LOOK_BODY2,sd->vd.body_style);

	//Update skill tree.
	pc->calc_skilltree(sd);
	clif->skillinfoblock(sd);

	if (sd->ed)
		elemental->delete(sd->ed, 0);
	if (sd->state.vending)
		vending->close(sd);

	map->foreachinmap(pc->jobchange_killclone, sd->bl.m, BL_MOB, sd->bl.id);

	//Remove peco/cart/falcon
	i = sd->sc.option;
	if (i&OPTION_RIDING && (!pc->checkskill(sd, KN_RIDING) || (sd->job & MAPID_THIRDMASK) == MAPID_RUNE_KNIGHT))
		i&=~OPTION_RIDING;
	if( i&OPTION_FALCON && !pc->checkskill(sd, HT_FALCON) )
		i&=~OPTION_FALCON;
	if( i&OPTION_DRAGON && !pc->checkskill(sd,RK_DRAGONTRAINING) )
		i&=~OPTION_DRAGON;
	if( i&OPTION_WUGRIDER && !pc->checkskill(sd,RA_WUGMASTERY) )
		i&=~OPTION_WUGRIDER;
	if( i&OPTION_WUG && !pc->checkskill(sd,RA_WUGMASTERY) )
		i&=~OPTION_WUG;
	if( i&OPTION_MADOGEAR ) //You do not need a skill for this.
		i&=~OPTION_MADOGEAR;
#ifndef NEW_CARTS
	if( i&OPTION_CART && !pc->checkskill(sd, MC_PUSHCART) )
		i&=~OPTION_CART;
#else
	if( sd->sc.data[SC_PUSH_CART] && !pc->checkskill(sd, MC_PUSHCART) )
		pc->setcart(sd, 0);
#endif
	if(i != sd->sc.option)
		pc->setoption(sd, i);

	if(homun_alive(sd->hd) && !pc->checkskill(sd, AM_CALLHOMUN))
		homun->vaporize(sd, HOM_ST_REST, true);

	if ((sd->sc.data[SC_SPRITEMABLE] && pc->checkskill(sd, SU_SPRITEMABLE)))
		status_change_end(&sd->bl, SC_SPRITEMABLE, INVALID_TIMER);

	if(sd->status.manner < 0)
		clif->changestatus(sd,SP_MANNER,sd->status.manner);

	status_calc_pc(sd,SCO_FORCE);
	pc->checkallowskill(sd);
	pc->equiplookall(sd);
	pc->update_job_and_level(sd);

	//if you were previously famous, not anymore.
	if (fame_flag != 0) {
		chrif->save(sd,0);
		chrif->buildfamelist();
	} else if (sd->status.fame > 0) {
		//It may be that now they are famous?
		switch (sd->job & MAPID_UPPERMASK) {
		case MAPID_BLACKSMITH:
		case MAPID_ALCHEMIST:
		case MAPID_TAEKWON:
			chrif->save(sd,0);
			chrif->buildfamelist();
		break;
		}
	}
	quest->questinfo_refresh(sd);

	achievement->validate_jobchange(sd); // Achievements [Smokexyz/Hercules]

	return 0;
}

/*==========================================
 * Tell client player sd has change equipement
 *------------------------------------------*/
static int pc_equiplookall(struct map_session_data *sd)
{
	nullpo_ret(sd);

	clif->changelook(&sd->bl,LOOK_WEAPON,0);
	clif->changelook(&sd->bl,LOOK_SHOES,0);
	clif->changelook(&sd->bl, LOOK_HEAD_BOTTOM, sd->status.look.head_bottom);
	clif->changelook(&sd->bl, LOOK_HEAD_TOP, sd->status.look.head_top);
	clif->changelook(&sd->bl, LOOK_HEAD_MID, sd->status.look.head_mid);
	clif->changelook(&sd->bl, LOOK_ROBE, sd->status.look.robe);

	return 0;
}

/*==========================================
 * Tell client player sd has change look (hair,equip...)
 *------------------------------------------*/
static int pc_changelook(struct map_session_data *sd, int type, int val)
{
	nullpo_ret(sd);

	switch(type){
		case LOOK_BASE:
			status->set_viewdata(&sd->bl, val);
			clif->changelook(&sd->bl, LOOK_BASE, sd->vd.class);
			clif->changelook(&sd->bl, LOOK_WEAPON, sd->status.look.weapon);
			if (sd->vd.cloth_color)
				clif->changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
			if (sd->vd.body_style)
				clif->changelook(&sd->bl,LOOK_BODY2,sd->vd.body_style);
			clif->skillinfoblock(sd);
			return 0;
			break;
		case LOOK_HAIR: //Use the battle_config limits! [Skotlex]
			val = cap_value(val, MIN_HAIR_STYLE, MAX_HAIR_STYLE);

			if (sd->status.hair != val) {
				sd->status.hair=val;
				if (sd->status.guild_id) //Update Guild Window. [Skotlex]
					intif->guild_change_memberinfo(sd->status.guild_id,sd->status.account_id,sd->status.char_id,
					GMI_HAIR,&sd->status.hair,sizeof(sd->status.hair));
			}
			break;
		case LOOK_WEAPON:
			sd->status.look.weapon = val;
			break;
		case LOOK_HEAD_BOTTOM:
			sd->status.look.head_bottom = val;
			break;
		case LOOK_HEAD_TOP:
			sd->status.look.head_top = val;
			break;
		case LOOK_HEAD_MID:
			sd->status.look.head_mid = val;
			break;
		case LOOK_HAIR_COLOR: //Use the battle_config limits! [Skotlex]
			val = cap_value(val, MIN_HAIR_COLOR, MAX_HAIR_COLOR);

			if (sd->status.hair_color != val) {
				sd->status.hair_color=val;
				if (sd->status.guild_id) //Update Guild Window. [Skotlex]
					intif->guild_change_memberinfo(sd->status.guild_id,sd->status.account_id,sd->status.char_id,
					GMI_HAIR_COLOR,&sd->status.hair_color,sizeof(sd->status.hair_color));
			}
			break;
		case LOOK_CLOTHES_COLOR: //Use the battle_config limits! [Skotlex]
			val = cap_value(val, MIN_CLOTH_COLOR, MAX_CLOTH_COLOR);

			sd->status.clothes_color=val;
			break;
		case LOOK_SHIELD:
			sd->status.look.shield = val;
			break;
		case LOOK_SHOES:
			break;
		case LOOK_ROBE:
			sd->status.look.robe = val;
			break;
		case LOOK_BODY2:
			val = cap_value(val, MIN_BODY_STYLE, MAX_BODY_STYLE);
			sd->status.body=val;
			break;
	}
	clif->changelook(&sd->bl,type,val);
	return 0;
}

/**
 * Hides a character.
 *
 * @param sd The character to hide.
 * @param show_msg Whether to show message to the character or not.
 *
 **/
static void pc_hide(struct map_session_data *sd, bool show_msg)
{
	nullpo_retv(sd);

	clif->clearunit_area(&sd->bl, CLR_OUTSIGHT);
	sd->sc.option |= OPTION_INVISIBLE;
	sd->vd.class = INVISIBLE_CLASS;

	if (show_msg)
		clif->message(sd->fd, atcommand->msgsd(sd, 11)); // Invisible: On

	// Decrement the number of pvp players on the map.
	map->list[sd->bl.m].users_pvp--;

	if (map->list[sd->bl.m].flag.pvp != 0 && map->list[sd->bl.m].flag.pvp_nocalcrank == 0
	    && sd->pvp_timer != INVALID_TIMER) { // Unregister the player for ranking.
		timer->delete(sd->pvp_timer, pc->calc_pvprank_timer);
		sd->pvp_timer = INVALID_TIMER;
	}

	clif->changeoption(&sd->bl);
}

/**
 * Unhides a character.
 *
 * @param sd The character to unhide.
 * @param show_msg Whether to show message to the character or not.
 *
 **/
static void pc_unhide(struct map_session_data *sd, bool show_msg)
{
	nullpo_retv(sd);

	sd->sc.option &= ~OPTION_INVISIBLE;

	if (sd->disguise != -1)
		status->set_viewdata(&sd->bl, sd->disguise);
	else
		status->set_viewdata(&sd->bl, sd->status.class);

	if (show_msg)
		clif->message(sd->fd, atcommand->msgsd(sd, 10)); // Invisible: Off

	// Increment the number of pvp players on the map.
	map->list[sd->bl.m].users_pvp++;

	if (map->list[sd->bl.m].flag.pvp != 0 && map->list[sd->bl.m].flag.pvp_nocalcrank == 0) // Register the player for ranking.
		sd->pvp_timer = timer->add(timer->gettick() + 200, pc->calc_pvprank_timer, sd->bl.id, 0);

	// bugreport:2266
	map->foreachinmovearea(clif->insight, &sd->bl, AREA_SIZE, sd->bl.x, sd->bl.y, BL_ALL, &sd->bl);

	if (sd->disguise != -1)
		clif->spawn_unit(&sd->bl, AREA_WOS);

	clif->changeoption(&sd->bl);
}

/*==========================================
 * Give an option (type) to player (sd) and display it to client
 *------------------------------------------*/
static int pc_setoption(struct map_session_data *sd, int type)
{
	int p_type, new_look=0;
	nullpo_ret(sd);
	p_type = sd->sc.option;

	//Option has to be changed client-side before the class sprite or it won't always work (eg: Wedding sprite) [Skotlex]
	sd->sc.option=type;

	if ((p_type & OPTION_INVISIBLE) != 0 && (type & OPTION_INVISIBLE) == 0) // Unhide character.
		pc->unhide(sd, false);
	else if ((p_type & OPTION_INVISIBLE) == 0 && (type & OPTION_INVISIBLE) != 0) // Hide character.
		pc->hide(sd, false);
	else
		clif->changeoption(&sd->bl);

	if( (type&OPTION_RIDING && !(p_type&OPTION_RIDING)) || (type&OPTION_DRAGON && !(p_type&OPTION_DRAGON) && pc->checkskill(sd,RK_DRAGONTRAINING) > 0) ) {
		// Mounting
		clif->sc_load(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_RIDING), 0, 0, 0);
		status_calc_pc(sd,SCO_NONE);
	} else if( (!(type&OPTION_RIDING) && p_type&OPTION_RIDING) || (!(type&OPTION_DRAGON) && p_type&OPTION_DRAGON) ) {
		// Dismount
		clif->sc_end(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_RIDING));
		status_calc_pc(sd,SCO_NONE);
	}

#ifndef NEW_CARTS
	if( type&OPTION_CART && !( p_type&OPTION_CART ) ) { //Cart On
		clif->cartList(sd);
		clif->updatestatus(sd, SP_CARTINFO);
		if(pc->checkskill(sd, MC_PUSHCART) < 10)
			status_calc_pc(sd,SCO_NONE); //Apply speed penalty.
	} else if( !( type&OPTION_CART ) && p_type&OPTION_CART ){ //Cart Off
		clif->clearcart(sd->fd);
		if(pc->checkskill(sd, MC_PUSHCART) < 10)
			status_calc_pc(sd,SCO_NONE); //Remove speed penalty.
		if ( sd->equip_index[EQI_AMMO] > 0 )
			pc->unequipitem(sd, sd->equip_index[EQI_AMMO], PCUNEQUIPITEM_FORCE);
	}
#endif

	if (type&OPTION_FALCON && !(p_type&OPTION_FALCON)) //Falcon ON
		clif->sc_load(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_FALCON), 0, 0, 0);
	else if (!(type&OPTION_FALCON) && p_type&OPTION_FALCON) //Falcon OFF
		clif->sc_end(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_FALCON));

	if( type&OPTION_WUGRIDER && !(p_type&OPTION_WUGRIDER) ) { // Mounting
		clif->sc_load(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_WUGRIDER), 0, 0, 0);
		status_calc_pc(sd,SCO_NONE);
	} else if( !(type&OPTION_WUGRIDER) && p_type&OPTION_WUGRIDER ) { // Dismount
		clif->sc_end(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_WUGRIDER));
		status_calc_pc(sd,SCO_NONE);
	}

	if( (type&OPTION_MADOGEAR && !(p_type&OPTION_MADOGEAR))
	|| (!(type&OPTION_MADOGEAR) && p_type&OPTION_MADOGEAR) ) {
		int i;
		status_calc_pc(sd, SCO_NONE);

		// End all SCs that can be reset when mado is taken off
		for( i = 0; i < SC_MAX; i++ ) {
			if ( !sd->sc.data[i] || !status->get_sc_type(i) )
				continue;
			if ( status->get_sc_type(i)&SC_MADO_NO_RESET )
				continue;
			switch (i) {
				case SC_BERSERK:
				case SC_SATURDAY_NIGHT_FEVER:
					sd->sc.data[i]->val2 = 0;
					break;
			}
			status_change_end(&sd->bl, (sc_type)i, INVALID_TIMER);
		}
		if ( sd->equip_index[EQI_AMMO] > 0 )
			pc->unequipitem(sd, sd->equip_index[EQI_AMMO], PCUNEQUIPITEM_FORCE);
	}

	if (type&OPTION_FLYING && !(p_type&OPTION_FLYING))
		new_look = JOB_STAR_GLADIATOR2;
	else if (!(type&OPTION_FLYING) && p_type&OPTION_FLYING)
		new_look = -1;

	if (sd->disguise != -1 || !new_look)
		return 0; //Disguises break sprite changes

	if (new_look < 0) { //Restore normal look.
		status->set_viewdata(&sd->bl, sd->status.class);
		new_look = sd->vd.class;
	}

	pc_stop_attack(sd); //Stop attacking on new view change (to prevent wedding/santa attacks.
	clif->changelook(&sd->bl,LOOK_BASE,new_look);
	if (sd->vd.cloth_color)
		clif->changelook(&sd->bl,LOOK_CLOTHES_COLOR,sd->vd.cloth_color);
	if( sd->vd.body_style )
		clif->changelook(&sd->bl,LOOK_BODY2,sd->vd.body_style);
	clif->skillinfoblock(sd); // Skill list needs to be updated after base change.

	return 0;
}

/*==========================================
 * Give player a cart
 *------------------------------------------*/
static int pc_setcart(struct map_session_data *sd, int type)
{
#ifndef NEW_CARTS
	int cart[6] = {OPTION_NOTHING,OPTION_CART1,OPTION_CART2,OPTION_CART3,OPTION_CART4,OPTION_CART5};
	int option;
#endif
	nullpo_ret(sd);

	if( type < 0 || type > MAX_CARTS )
		return 1;// Never trust the values sent by the client! [Skotlex]

	if( pc->checkskill(sd,MC_PUSHCART) <= 0 && type != 0 )
		return 1;// Push cart is required

	if( type == 0 && pc_iscarton(sd) )
		status_change_end(&sd->bl,SC_GN_CARTBOOST,INVALID_TIMER);

#ifdef NEW_CARTS

	switch( type ) {
		case 0:
			if( !sd->sc.data[SC_PUSH_CART] )
				return 0;
			status_change_end(&sd->bl,SC_PUSH_CART,INVALID_TIMER);
			clif->clearcart(sd->fd);
			clif->updatestatus(sd, SP_CARTINFO);
			if ( sd->equip_index[EQI_AMMO] > 0 )
				pc->unequipitem(sd, sd->equip_index[EQI_AMMO], PCUNEQUIPITEM_FORCE);
			break;
		default:/* everything else is an allowed ID so we can move on */
			if( !sd->sc.data[SC_PUSH_CART] ) /* first time, so fill cart data */
				clif->cartList(sd);
			clif->updatestatus(sd, SP_CARTINFO);
		        sc_start(NULL, &sd->bl, SC_PUSH_CART, 100, type, 0, MC_PUSHCART);
			clif->sc_load(&sd->bl, sd->bl.id, AREA, status->get_sc_icon(SC_ON_PUSH_CART), type, 0, 0);
			if( sd->sc.data[SC_PUSH_CART] )/* forcefully update */
				sd->sc.data[SC_PUSH_CART]->val1 = type;
			break;
	}

	if(pc->checkskill(sd, MC_PUSHCART) < 10)
		status_calc_pc(sd,SCO_NONE); //Recalc speed penalty.
#else
	// Update option
	option = sd->sc.option;
	option &= ~OPTION_CART;// clear cart bits
	option |= cart[type]; // set cart
	pc->setoption(sd, option);
#endif

	return 0;
}

/* FIXME: These setter methods are inconsistent in their class/skill checks.
 *        They should be changed so that they all either do or skip the checks.*/

/**
 * Gives/removes a falcon.
 *
 * The target player needs the required skills in order to obtain a falcon.
 *
 * @param sd Target player.
 * @param flag New state.
 **/
static void pc_setfalcon(struct map_session_data *sd, bool flag)
{
	nullpo_retv(sd);
	if (flag) {
		if (pc->checkskill(sd,HT_FALCON) > 0) // add falcon if he have the skill
			pc->setoption(sd,sd->sc.option|OPTION_FALCON);
	} else if (pc_isfalcon(sd)) {
		pc->setoption(sd,sd->sc.option&~OPTION_FALCON); // remove falcon
	}
}

/**
 * Mounts/dismounts a Peco or Gryphon.
 *
 * The target player needs the required skills in order to mount a peco.
 *
 * @param sd Target player.
 * @param flag New state.
 **/
static void pc_setridingpeco(struct map_session_data *sd, bool flag)
{
	nullpo_retv(sd);
	if (flag) {
		if (pc->checkskill(sd, KN_RIDING))
			pc->setoption(sd, sd->sc.option|OPTION_RIDING);
	} else if (pc_isridingpeco(sd)) {
		pc->setoption(sd, sd->sc.option&~OPTION_RIDING);
	}
}

/**
 * Gives/removes a Mado Gear.
 *
 * The target player needs to be the correct class in order to obtain a mado gear.
 *
 * @param sd Target player.
 * @param flag New state.
 * @param mtype Type of the mado gear.
 **/
static void pc_setmadogear(struct map_session_data *sd, bool flag, enum mado_type mtype)
{
	nullpo_retv(sd);
	Assert_retv(mtype >= MADO_ROBOT && mtype < MADO_MAX);

	if (flag) {
		if ((sd->job & MAPID_THIRDMASK) == MAPID_MECHANIC) {
			pc->setoption(sd, sd->sc.option|OPTION_MADOGEAR);
#if PACKETVER_MAIN_NUM >= 20191120 || PACKETVER_RE_NUM >= 20191106
			sc_start(&sd->bl, &sd->bl, SC_MADOGEAR, 100, (int)mtype, INFINITE_DURATION, 0);
#endif
		}
	} else if (pc_ismadogear(sd)) {
		pc->setoption(sd, sd->sc.option&~OPTION_MADOGEAR);
		// pc->setoption resets status effects when changing mado, no need to re do it here.
	}
}

/**
 * Mounts/dismounts a dragon.
 *
 * The target player needs the required skills in order to mount a dragon.
 *
 * @param sd Target player.
 * @param type New state. This must be a valid OPTION_DRAGON* or 0.
 **/
static void pc_setridingdragon(struct map_session_data *sd, unsigned int type)
{
	nullpo_retv(sd);
	if (type&OPTION_DRAGON) {
		// Ensure only one dragon is set at a time.
		if (type&OPTION_DRAGON1)
			type = OPTION_DRAGON1;
		else if (type&OPTION_DRAGON2)
			type = OPTION_DRAGON2;
		else if (type&OPTION_DRAGON3)
			type = OPTION_DRAGON3;
		else if (type&OPTION_DRAGON4)
			type = OPTION_DRAGON4;
		else if (type&OPTION_DRAGON5)
			type = OPTION_DRAGON5;
		else
			type = OPTION_DRAGON1;

		if (pc->checkskill(sd, RK_DRAGONTRAINING))
			pc->setoption(sd, (sd->sc.option&~OPTION_DRAGON)|type);
	} else if (pc_isridingdragon(sd)) {
		pc->setoption(sd,sd->sc.option&~OPTION_DRAGON); // remove dragon
	}
}

/**
 * Mounts/dismounts a wug.
 *
 * The target player needs the required skills in order to mount a wug.
 *
 * @param sd Target player.
 * @param flag New state.
 **/
static void pc_setridingwug(struct map_session_data *sd, bool flag)
{
	nullpo_retv(sd);
	if (flag) {
		if (pc->checkskill(sd, RA_WUGRIDER) > 0)
			pc->setoption(sd,sd->sc.option|OPTION_WUGRIDER);
	} else if (pc_isridingwug(sd)) {
		pc->setoption(sd,sd->sc.option&~OPTION_WUGRIDER); // remove wug
	}
}

/**
 * Determines whether a player can attack based on status changes
 *  Why not use status_check_skilluse?
 *  "src MAY be null to indicate we shouldn't check it, this is a ground-based skill attack."
 *  Even ground-based attacks should be blocked by these statuses
 * Called from unit_attack and unit_attack_timer_sub
 * @retval true Can attack
 **/
static bool pc_can_attack(struct map_session_data *sd, int target_id)
{
	nullpo_retr(false, sd);

	if( sd->sc.data[SC_BASILICA] ||
		sd->sc.data[SC__SHADOWFORM] ||
		sd->sc.data[SC__MANHOLE] ||
		sd->sc.data[SC_CURSEDCIRCLE_ATKER] ||
		sd->sc.data[SC_CURSEDCIRCLE_TARGET] ||
		sd->sc.data[SC_COLD] ||
		sd->sc.data[SC_ALL_RIDING] || // The client doesn't let you, this is to make cheat-safe
		sd->sc.data[SC_TRICKDEAD] ||
		(sd->sc.data[SC_SIREN] && sd->sc.data[SC_SIREN]->val2 == target_id) ||
		sd->sc.data[SC_BLADESTOP] ||
		sd->sc.data[SC_DEEP_SLEEP] ||
		sd->sc.data[SC_FALLENEMPIRE] ||
		sd->block_action.attack)
			return false;

	return true;
}

/**
 * Determines whether a player can talk/whisper based on status changes
 * Called from clif_parse_GlobalMessage and clif_parse_WisMessage
 * @retval true Can talk
 **/
static bool pc_can_talk(struct map_session_data *sd)
{
	nullpo_retr(false, sd);

	if( sd->sc.data[SC_BERSERK] ||
		(sd->sc.data[SC_DEEP_SLEEP] && sd->sc.data[SC_DEEP_SLEEP]->val2) ||
		pc_ismuted(&sd->sc, MANNER_NOCHAT) ||
		sd->block_action.chat)
		return false;

	return true;
}

/*==========================================
 * Check if player can drop an item
 *------------------------------------------*/
static int pc_candrop(struct map_session_data *sd, struct item *item)
{
	if( item && (item->expire_time || (item->bound && !pc_can_give_bound_items(sd))) )
		return 0;
	if( !pc_can_give_items(sd) ) //check if this GM level can drop items
		return 0;
	return (itemdb_isdropable(item, pc_get_group_level(sd)));
}
/**
 * For '@type' variables (temporary numeric char reg)
 **/
static int pc_readreg(struct map_session_data *sd, int64 reg)
{
	nullpo_ret(sd);
	return i64db_iget(sd->regs.vars, reg);
}
/**
 * For '@type' variables (temporary numeric char reg)
 **/
static void pc_setreg(struct map_session_data *sd, int64 reg, int val)
{
	unsigned int index = script_getvaridx(reg);

	nullpo_retv(sd);
	if( val ) {
		i64db_iput(sd->regs.vars, reg, val);
		if( index )
			script->array_update(&sd->regs, reg, false);
	} else {
		i64db_remove(sd->regs.vars, reg);
		if( index )
			script->array_update(&sd->regs, reg, true);
	}
}

/**
 * For '@type$' variables (temporary string char reg)
 **/
static char *pc_readregstr(struct map_session_data *sd, int64 reg)
{
	struct script_reg_str *p = NULL;

	nullpo_retr(NULL, sd);
	p = i64db_get(sd->regs.vars, reg);

	return p ? p->value : NULL;
}
/**
 * For '@type$' variables (temporary string char reg)
 **/
static void pc_setregstr(struct map_session_data *sd, int64 reg, const char *str)
{
	struct script_reg_str *p = NULL;
	unsigned int index = script_getvaridx(reg);
	struct DBData prev;

	nullpo_retv(sd);
	nullpo_retv(str);
	if( str[0] ) {
		p = ers_alloc(pc->str_reg_ers, struct script_reg_str);

		p->value = aStrdup(str);
		p->flag.type = 1;

		if( sd->regs.vars->put(sd->regs.vars, DB->i642key(reg), DB->ptr2data(p), &prev) ) {
			p = DB->data2ptr(&prev);
			if( p->value )
				aFree(p->value);
			ers_free(pc->str_reg_ers, p);
		} else {
			if( index )
				script->array_update(&sd->regs, reg, false);
		}
	} else {
		if( sd->regs.vars->remove(sd->regs.vars, DB->i642key(reg), &prev) ) {
			p = DB->data2ptr(&prev);
			if( p->value )
				aFree(p->value);
			ers_free(pc->str_reg_ers, p);
			if( index )
				script->array_update(&sd->regs, reg, true);
		}
	}
}
/**
 * Serves the following variable types:
 * - 'type' (permanent nuneric char reg)
 * - '#type' (permanent numeric account reg)
 * - '##type' (permanent numeric account reg2)
 **/
static int pc_readregistry(struct map_session_data *sd, int64 reg)
{
	struct script_reg_num *p = NULL;

	nullpo_ret(sd);
	if (!sd->vars_ok) {
		ShowError("pc_readregistry: Trying to read reg %s before it's been loaded!\n", script->get_str(script_getvarid(reg)));
		//This really shouldn't happen, so it's possible the data was lost somewhere, we should request it again.
		//intif->request_registry(sd,type==3?4:type);
		sockt->eof(sd->fd);
		return 0;
	}

	p = i64db_get(sd->regs.vars, reg);

	return p ? p->value : 0;
}
/**
 * Serves the following variable types:
 * - 'type$' (permanent str char reg)
 * - '#type$' (permanent str account reg)
 * - '##type$' (permanent str account reg2)
 **/
static char *pc_readregistry_str(struct map_session_data *sd, int64 reg)
{
	struct script_reg_str *p = NULL;

	nullpo_retr(NULL, sd);
	if (!sd->vars_ok) {
		ShowError("pc_readregistry_str: Trying to read reg %s before it's been loaded!\n", script->get_str(script_getvarid(reg)));
		//This really shouldn't happen, so it's possible the data was lost somewhere, we should request it again.
		//intif->request_registry(sd,type==3?4:type);
		sockt->eof(sd->fd);
		return NULL;
	}

	p = i64db_get(sd->regs.vars, reg);

	return p ? p->value : NULL;
}
/**
 * Serves the following variable types:
 * - 'type' (permanent nuneric char reg)
 * - '#type' (permanent numeric account reg)
 * - '##type' (permanent numeric account reg2)
 **/
static int pc_setregistry(struct map_session_data *sd, int64 reg, int val)
{
	struct script_reg_num *p = NULL;
	const char *regname = script->get_str( script_getvarid(reg) );
	unsigned int index = script_getvaridx(reg);

	nullpo_ret(sd);
	/* SAAD! those things should be stored elsewhere e.g. char ones in char table, the cash ones in account_data table! */
	switch( regname[0] ) {
		default: //Char reg
			if( !strcmp(regname,"PC_DIE_COUNTER") && sd->die_counter != val ) {
				int i = (!sd->die_counter && (sd->job & MAPID_UPPERMASK) == MAPID_SUPER_NOVICE);
				sd->die_counter = val;
				if( i )
					status_calc_pc(sd,SCO_NONE); // Lost the bonus.
			} else if( !strcmp(regname,"COOK_MASTERY") && sd->cook_mastery != val ) {
				val = cap_value(val, 0, 1999);
				sd->cook_mastery = val;
			}
			break;
		case '#':
			if( !strcmp(regname,"#CASHPOINTS") && sd->cashPoints != val ) {
				val = cap_value(val, 0, MAX_ZENY);
				sd->cashPoints = val;
			} else if( !strcmp(regname,"#KAFRAPOINTS") && sd->kafraPoints != val ) {
				val = cap_value(val, 0, MAX_ZENY);
				sd->kafraPoints = val;
			} else if (strcmp(regname, GOLDPC_POINTS_VAR) == 0 && sd->goldpc.points != val) {
				bool is_full = (sd->goldpc.points == GOLDPC_MAX_POINTS);
				val = cap_value(val, 0, GOLDPC_MAX_POINTS);
				sd->goldpc.points = val;

				if (sd->goldpc.loaded) {
					if (is_full)
						goldpc->start(sd);
					else
						clif->goldpc_info(sd);
				}
			}
			break;
	}

	if ( !pc->reg_load && !sd->vars_ok ) {
		ShowError("pc_setregistry : refusing to set %s until vars are received.\n", regname);
		return 0;
	}

	if( (p = i64db_get(sd->regs.vars, reg) ) ) {
		if( val ) {
			if( !p->value && index ) /* its a entry that was deleted, so we reset array */
				script->array_update(&sd->regs, reg, false);
			p->value = val;
		} else {
			p->value = 0;
			if( index )
				script->array_update(&sd->regs, reg, true);
		}
		if( !pc->reg_load )
			p->flag.update = 1;/* either way, it will require either delete or replace */
	} else if( val ) {
		struct DBData prev;

		if( index )
			script->array_update(&sd->regs, reg, false);

		p = ers_alloc(pc->num_reg_ers, struct script_reg_num);

		p->value = val;
		if( !pc->reg_load )
			p->flag.update = 1;

		if( sd->regs.vars->put(sd->regs.vars, DB->i642key(reg), DB->ptr2data(p), &prev) ) {
			p = DB->data2ptr(&prev);
			ers_free(pc->num_reg_ers, p);
		}
	}

	if( !pc->reg_load && p )
		sd->vars_dirty = true;

	return 1;
}
/**
 * Serves the following variable types:
 * - 'type$' (permanent str char reg)
 * - '#type$' (permanent str account reg)
 * - '##type$' (permanent str account reg2)
 **/
static int pc_setregistry_str(struct map_session_data *sd, int64 reg, const char *val)
{
	struct script_reg_str *p = NULL;
	const char *regname = script->get_str( script_getvarid(reg) );
	unsigned int index = script_getvaridx(reg);

	nullpo_ret(sd);
	nullpo_ret(val);
	if ( !pc->reg_load && !sd->vars_ok ) {
		ShowError("pc_setregistry_str : refusing to set %s until vars are received.\n", regname);
		return 0;
	}

	if( (p = i64db_get(sd->regs.vars, reg) ) ) {
		if( val[0] ) {
			if( p->value )
				aFree(p->value);
			else if ( index ) /* a entry that was deleted, so we reset */
				script->array_update(&sd->regs, reg, false);
			p->value = aStrdup(val);
		} else {
			p->value = NULL;
			if( index )
				script->array_update(&sd->regs, reg, true);
		}
		if( !pc->reg_load )
			p->flag.update = 1;/* either way, it will require either delete or replace */
	} else if( val[0] ) {
		struct DBData prev;

		if( index )
			script->array_update(&sd->regs, reg, false);

		p = ers_alloc(pc->str_reg_ers, struct script_reg_str);

		p->value = aStrdup(val);
		if( !pc->reg_load )
			p->flag.update = 1;
		p->flag.type = 1;

		if( sd->regs.vars->put(sd->regs.vars, DB->i642key(reg), DB->ptr2data(p), &prev) ) {
			p = DB->data2ptr(&prev);
			if( p->value )
				aFree(p->value);
			ers_free(pc->str_reg_ers, p);
		}
	}

	if( !pc->reg_load && p )
		sd->vars_dirty = true;

	return 1;
}

/*==========================================
 * Exec eventtimer for player sd (retrieved from map_session (id))
 *------------------------------------------*/
static int pc_eventtimer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd=map->id2sd(id);
	char *p = (char *)data;
	int i;
	if(sd==NULL)
		return 0;

	ARR_FIND( 0, MAX_EVENTTIMER, i, sd->eventtimer[i] == tid );
	if( i < MAX_EVENTTIMER )
	{
		sd->eventtimer[i] = INVALID_TIMER;
		sd->eventcount--;
		npc->event(sd,p,0);
	}
	else
		ShowError("pc_eventtimer: no such event timer\n");

	if (p) aFree(p);
	return 0;
}

/*==========================================
 * Add eventtimer for player sd ?
 *------------------------------------------*/
static int pc_addeventtimer(struct map_session_data *sd, int tick, const char *name)
{
	int i;
	nullpo_ret(sd);
	nullpo_ret(name);

	ARR_FIND( 0, MAX_EVENTTIMER, i, sd->eventtimer[i] == INVALID_TIMER );
	if( i == MAX_EVENTTIMER )
		return 0;

	sd->eventtimer[i] = timer->add(timer->gettick()+tick, pc->eventtimer, sd->bl.id, (intptr_t)aStrdup(name));
	sd->eventcount++;

	return 1;
}

/*==========================================
 * Del eventtimer for player sd ?
 *------------------------------------------*/
static int pc_deleventtimer(struct map_session_data *sd, const char *name)
{
	char* p = NULL;
	int i;

	nullpo_ret(sd);
	nullpo_ret(name);

	if (sd->eventcount <= 0)
		return 0;

	// find the named event timer
	ARR_FIND( 0, MAX_EVENTTIMER, i,
		sd->eventtimer[i] != INVALID_TIMER &&
		(p = (char *)(timer->get(sd->eventtimer[i])->data)) != NULL &&
		strcmp(p, name) == 0
	);
	if( i == MAX_EVENTTIMER )
		return 0; // not found

	timer->delete(sd->eventtimer[i],pc->eventtimer);
	sd->eventtimer[i] = INVALID_TIMER;
	sd->eventcount--;
	aFree(p);

	return 1;
}

/*==========================================
 * Update eventtimer count for player sd
 *------------------------------------------*/
static int pc_addeventtimercount(struct map_session_data *sd, const char *name, int tick)
{
	int i;

	nullpo_ret(sd);

	for(i=0;i<MAX_EVENTTIMER;i++)
		if( sd->eventtimer[i] != INVALID_TIMER && strcmp(
			(char *)(timer->get(sd->eventtimer[i])->data), name)==0 ){
				timer->addtick(sd->eventtimer[i],tick);
				break;
		}

	return 0;
}

/*==========================================
 * Remove all eventtimer for player sd
 *------------------------------------------*/
static int pc_cleareventtimer(struct map_session_data *sd)
{
	int i;

	nullpo_ret(sd);

	if (sd->eventcount <= 0)
		return 0;

	for(i=0;i<MAX_EVENTTIMER;i++)
		if( sd->eventtimer[i] != INVALID_TIMER ){
			char *p = (char *)(timer->get(sd->eventtimer[i])->data);
			timer->delete(sd->eventtimer[i],pc->eventtimer);
			sd->eventtimer[i] = INVALID_TIMER;
			sd->eventcount--;
			if (p) aFree(p);
		}
	return 0;
}
/* called when a item with combo is worn */
static int pc_checkcombo(struct map_session_data *sd, struct item_data *data)
{
	int i, j, k, z;
	int index, success = 0;
	struct pc_combos *combo;

	nullpo_ret(sd);
	nullpo_ret(data);
	for( i = 0; i < data->combos_count; i++ ) {

		/* ensure this isn't a duplicate combo */
		if( sd->combos != NULL ) {
			int x;

			ARR_FIND( 0, sd->combo_count, x, sd->combos[x].id == data->combos[i]->id );

			/* found a match, skip this combo */
			if( x < sd->combo_count )
				continue;
		}

		for( j = 0; j < data->combos[i]->count; j++ ) {
			int id = data->combos[i]->nameid[j];
			bool found = false;

			for( k = 0; k < EQI_MAX; k++ ) {
				index = sd->equip_index[k];
				if( index < 0 ) continue;
				if( k == EQI_HAND_R   &&  sd->equip_index[EQI_HAND_L] == index ) continue;
				if( k == EQI_HEAD_MID &&  sd->equip_index[EQI_HEAD_LOW] == index ) continue;
				if( k == EQI_HEAD_TOP && (sd->equip_index[EQI_HEAD_MID] == index || sd->equip_index[EQI_HEAD_LOW] == index) ) continue;

				if(!sd->inventory_data[index])
					continue;

				if ( itemdb_type(id) != IT_CARD ) {
					if ( sd->inventory_data[index]->nameid != id )
						continue;

					found = true;
					break;
				} else { //Cards
					if ( sd->inventory_data[index]->slot == 0 || itemdb_isspecial(sd->status.inventory[index].card[0]) )
						continue;

					for (z = 0; z < sd->inventory_data[index]->slot; z++) {

						if (sd->status.inventory[index].card[z] != id)
							continue;

						// We have found a match
						found = true;
						break;
					}
				}

			}

			if( !found )
				break;/* we haven't found all the ids for this combo, so we can return */
		}

		/* means we broke out of the count loop w/o finding all ids, we can move to the next combo */
		if( j < data->combos[i]->count )
			continue;

		/* we got here, means all items in the combo are matching */

		RECREATE(sd->combos, struct pc_combos, ++sd->combo_count);
		combo = &sd->combos[sd->combo_count - 1];
		combo->bonus = data->combos[i]->script;
		combo->id = data->combos[i]->id;

		success++;
	}

	return success;
}

/* called when a item with combo is removed */
static int pc_removecombo(struct map_session_data *sd, struct item_data *data)
{
	int i, retval = 0;

	nullpo_ret(sd);
	nullpo_ret(data);
	if( !sd->combos )
		return 0;/* nothing to do here, player has no combos */

	for( i = 0; i < data->combos_count; i++ ) {
		/* check if this combo exists in this user */
		int x = 0, cursor = 0, j;

		ARR_FIND( 0, sd->combo_count, x, sd->combos[x].id == data->combos[i]->id );
		/* no match, skip this combo */
		if( x == sd->combo_count )
			continue;

		sd->combos[x].bonus = NULL;
		sd->combos[x].id = 0;

		retval++;

		for( j = 0, cursor = 0; j < sd->combo_count; j++ ) {
			if( sd->combos[j].bonus == NULL )
				continue;

			if( cursor != j ) {
				sd->combos[cursor].bonus = sd->combos[j].bonus;
				sd->combos[cursor].id    = sd->combos[j].id;
			}

			cursor++;
		}

		/* it's empty, we can clear all the memory */
		if( (sd->combo_count = cursor) == 0 ) {
			aFree(sd->combos);
			sd->combos = NULL;
			break;
		}
	}

	/* check if combo requirements still fit -- don't touch retval! */
	pc->checkcombo( sd, data );

	return retval;
}
static int pc_load_combo(struct map_session_data *sd)
{
	int i, ret = 0;
	nullpo_ret(sd);
	for( i = 0; i < EQI_MAX; i++ ) {
		struct item_data *id = NULL;
		int idx = sd->equip_index[i];
		if( sd->equip_index[i] < 0 || !(id = sd->inventory_data[idx] ) )
			continue;
		if( id->combos_count )
			ret += pc->checkcombo(sd,id);
		if(!itemdb_isspecial(sd->status.inventory[idx].card[0])) {
			struct item_data *data;
			int j;
			for( j = 0; j < id->slot; j++ ) {
				if (!sd->status.inventory[idx].card[j])
					continue;
				if ( ( data = itemdb->exists(sd->status.inventory[idx].card[j]) ) != NULL ) {
					if( data->combos_count )
						ret += pc->checkcombo(sd,data);
				}
			}
		}
	}
	return ret;
}

/**
 * Equip item at given position.
 * @param sd the affected player structure. Must be checked before.
 * @param id item structure for equip. Must be checked before.
 * @param n inventory item position. Must be checked before.
 * @param pos slot position. Must be checked before.
 */
static void pc_equipitem_pos(struct map_session_data *sd, struct item_data *id, int n, int pos)
{
	nullpo_retv(sd);
	if ((!map_no_view(sd->bl.m,EQP_SHADOW_WEAPON) && pos & EQP_SHADOW_WEAPON) ||
		 (pos & EQP_HAND_R)) {
		if (id != NULL) {
			sd->weapontype1 = id->subtype;
			sd->status.look.weapon = id->view_sprite;
		} else {
			sd->weapontype1 = W_FIST;
			sd->status.look.weapon = 0;
		}
		pc->calcweapontype(sd);
		clif->changelook(&sd->bl, LOOK_WEAPON, sd->status.look.weapon);
	}
	if ((!map_no_view(sd->bl.m,EQP_SHADOW_SHIELD) && pos & EQP_SHADOW_SHIELD) ||
		 (pos & EQP_HAND_L)) {
		if (id != NULL) {
			if (id->type == IT_WEAPON) {
				sd->has_shield = false;
				sd->status.look.shield = 0;
				sd->weapontype2 = id->subtype;
			} else if (id->type == IT_ARMOR) {
				sd->has_shield = true;
				sd->status.look.shield = id->view_sprite;
				sd->weapontype2 = W_FIST;
			}
		} else {
			sd->has_shield = false;
			sd->status.look.shield = 0;
			sd->weapontype2 = W_FIST;
		}
		pc->calcweapontype(sd);
		clif->changelook(&sd->bl, LOOK_SHIELD, sd->status.look.shield);
	}
	//Added check to prevent sending the same look on multiple slots ->
	//causes client to redraw item on top of itself. (suggested by Lupus)
	if (!map_no_view(sd->bl.m,EQP_HEAD_LOW) && pos & EQP_HEAD_LOW && pc->checkequip(sd,EQP_COSTUME_HEAD_LOW) == -1) {
		if (id && !(pos&(EQP_HEAD_TOP|EQP_HEAD_MID)))
			sd->status.look.head_bottom = id->view_sprite;
		else
			sd->status.look.head_bottom = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_BOTTOM, sd->status.look.head_bottom);
	}
	if (!map_no_view(sd->bl.m,EQP_HEAD_TOP) && pos & EQP_HEAD_TOP && pc->checkequip(sd,EQP_COSTUME_HEAD_TOP) == -1) {
		if (id)
			sd->status.look.head_top = id->view_sprite;
		else
			sd->status.look.head_top = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_TOP, sd->status.look.head_top);
	}
	if (!map_no_view(sd->bl.m,EQP_HEAD_MID) && pos & EQP_HEAD_MID && pc->checkequip(sd,EQP_COSTUME_HEAD_MID) == -1) {
		if (id && !(pos&EQP_HEAD_TOP))
			sd->status.look.head_mid = id->view_sprite;
		else
			sd->status.look.head_mid = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_MID, sd->status.look.head_mid);
	}
	if (!map_no_view(sd->bl.m,EQP_COSTUME_HEAD_TOP) && pos & EQP_COSTUME_HEAD_TOP) {
		if (id){
			sd->status.look.head_top = id->view_sprite;
		} else
			sd->status.look.head_top = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_TOP, sd->status.look.head_top);
	}
	if (!map_no_view(sd->bl.m,EQP_COSTUME_HEAD_MID) && pos & EQP_COSTUME_HEAD_MID) {
		if(id && !(pos&EQP_HEAD_TOP)){
			sd->status.look.head_mid = id->view_sprite;
		} else
			sd->status.look.head_mid = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_MID, sd->status.look.head_mid);
	}
	if (!map_no_view(sd->bl.m,EQP_COSTUME_HEAD_LOW) && pos & EQP_COSTUME_HEAD_LOW) {
		if (id && !(pos&(EQP_HEAD_TOP|EQP_HEAD_MID))){
			sd->status.look.head_bottom = id->view_sprite;
		} else
			sd->status.look.head_bottom = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_BOTTOM, sd->status.look.head_bottom);
	}

	if (!map_no_view(sd->bl.m,EQP_SHOES) && pos & EQP_SHOES)
		clif->changelook(&sd->bl,LOOK_SHOES,0);
	if (!map_no_view(sd->bl.m,EQP_GARMENT) && pos&EQP_GARMENT && pc->checkequip(sd,EQP_COSTUME_GARMENT) == -1) {
		sd->status.look.robe = id ? id->view_sprite : 0;
		clif->changelook(&sd->bl, LOOK_ROBE, sd->status.look.robe);
	}

	if (!map_no_view(sd->bl.m,EQP_COSTUME_GARMENT) && pos & EQP_COSTUME_GARMENT) {
		sd->status.look.robe = id ? id->view_sprite : 0;
		clif->changelook(&sd->bl, LOOK_ROBE, sd->status.look.robe);
	}
}

/**
 * Attempts to equip an item.
 *
 * @param sd The related character.
 * @param n The item's inventory index.
 * @param req_pos The equipment slot, where the item should be equipped. (See enum equip_pos.)
 * @return 0 on failure, 1 on success.
 *
 **/
static int pc_equipitem(struct map_session_data *sd, int n, int req_pos)
{
	nullpo_ret(sd);

	if (n < 0 || n >= sd->status.inventorySize) {
		clif->equipitemack(sd, 0, 0, EIA_FAIL);
		return 0;
	}

	// If the character is in berserk mode, the item can't be equipped.
	if (sd->sc.count != 0 && (sd->sc.data[SC_BERSERK] != NULL || sd->sc.data[SC_NO_SWITCH_EQUIP] != NULL)) {
		clif->equipitemack(sd, n, 0, EIA_FAIL);
		return 0;
	}

	if (battle_config.battle_log != 0)
		ShowInfo("equip %d(%d) %x:%x\n", sd->status.inventory[n].nameid, n, sd->status.inventory[n].equip,
			 (unsigned int)req_pos);

	if (DIFF_TICK(sd->canequip_tick, timer->gettick()) > 0) {
		clif->equipitemack(sd, n, 0, EIA_FAIL);
		return 0;
	}

	int pos = pc->equippoint(sd, n); // With a few exceptions, item should go in all specified slots.

	if (pc->isequip(sd,n) == 0 || (pos & req_pos) == 0 || sd->status.inventory[n].equip != 0
	    || (sd->status.inventory[n].attribute & ATTR_BROKEN) != 0) {
		clif->equipitemack(sd, n, 0, EIA_FAIL);
		return 0;
	}

	if (sd->inventory_data[n]->flag.bindonequip != 0 && sd->status.inventory[n].bound == 0) {
		sd->status.inventory[n].bound = IBT_CHARACTER;
		clif->notify_bounditem(sd, n);
	}

	if (pos == EQP_ACC) { // Accesories should only go in one of the two.
		pos = req_pos & EQP_ACC;

		if (pos == EQP_ACC) // User specified both slots.
			pos = (sd->equip_index[EQI_ACC_R] >= 0) ? EQP_ACC_L : EQP_ACC_R;
	} else if (pos == EQP_ARMS && sd->inventory_data[n]->equip == EQP_HAND_R) { // Dual wield capable weapon.
		pos = req_pos & EQP_ARMS;

		if (pos == EQP_ARMS) // User specified both slots, pick one for them.
			pos = (sd->equip_index[EQI_HAND_R] >= 0) ? EQP_HAND_L : EQP_HAND_R;
	} else if (pos == EQP_SHADOW_ACC) { // Accesories should only go in one of the two,
		pos = req_pos & EQP_SHADOW_ACC;

		if (pos == EQP_SHADOW_ACC) // User specified both slots.
			pos = (sd->equip_index[EQI_SHADOW_ACC_R] >= 0) ? EQP_SHADOW_ACC_L : EQP_SHADOW_ACC_R;
	} else if (pos == EQP_SHADOW_ARMS && sd->inventory_data[n]->equip == EQP_SHADOW_WEAPON) { // Dual wield capable weapon.
		pos = req_pos & EQP_SHADOW_ARMS;

		if (pos == EQP_SHADOW_ARMS) // User specified both slots, pick one for them.
			pos = (sd->equip_index[EQI_SHADOW_WEAPON] >= 0) ? EQP_SHADOW_SHIELD : EQP_SHADOW_WEAPON;
	}

	int flag = 0;

	// Update skill-block range database when weapon range changes. [Skotlex]
	if ((pos & EQP_HAND_R) != 0 && (battle_config.use_weapon_skill_range & BL_PC) != 0) {
		int idx = sd->equip_index[EQI_HAND_R];

		if (idx < 0 || sd->inventory_data[idx] == NULL) // No data, or no weapon equipped.
			flag = 1;
		else
			flag = (sd->inventory_data[n]->range != sd->inventory_data[idx]->range) ? 1 : 0;
	}

	for (int i = 0; i < EQI_MAX; i++) {
		if ((pos & pc->equip_pos[i]) != 0) {
			if (sd->equip_index[i] >= 0) // Slot taken, remove item from there.
				pc->unequipitem(sd, sd->equip_index[i], PCUNEQUIPITEM_FORCE);

			sd->equip_index[i] = n;
		}
	}

	if (pos == EQP_AMMO) {
		clif->arrowequip(sd, n);
		clif->arrow_fail(sd, 3);
	} else {
		clif->equipitemack(sd, n, pos, EIA_SUCCESS);
	}

	sd->status.inventory[n].equip = pos;
	pc->equipitem_pos(sd, sd->inventory_data[n], n, pos);
	pc->checkallowskill(sd); // Check if status changes should be halted.

	int iflag = sd->npc_item_flag;

	// Check for combos. (MUST be done before status->calc_pc()!)
	if (sd->inventory_data[n]->combos_count != 0)
		pc->checkcombo(sd, sd->inventory_data[n]);

	if (!itemdb_isspecial(sd->status.inventory[n].card[0])) {
		for (int i = 0; i < sd->inventory_data[n]->slot; i++) {
			if (sd->status.inventory[n].card[i] == 0)
				continue;

			struct item_data *data = itemdb->exists(sd->status.inventory[n].card[i]);

			if (data != NULL && data->combos_count != 0)
				pc->checkcombo(sd, data);
		}
	}

	status_calc_pc(sd, SCO_NONE);

	if (flag != 0) // Update skill data.
		clif->skillinfoblock(sd);

	// Execute equip script. [Skotlex]
	struct item_data *equip_data = sd->inventory_data[n];
	struct map_zone_data *zone = map->list[sd->bl.m].zone;
	int dis_items_cnt = zone->disabled_items_count;

	if (equip_data->equip_script != NULL) {
		int idx;

		ARR_FIND(0, dis_items_cnt, idx, zone->disabled_items[idx] == equip_data->nameid);

		if (idx == dis_items_cnt)
			script->run_item_equip_script(sd, equip_data, npc->fake_nd->bl.id);
	}

	struct item *equip = &sd->status.inventory[n];

	if (!itemdb_isspecial(equip->card[0])) {
		for (int slot = 0; slot < equip_data->slot; slot++) {
			if (equip->card[slot] == 0)
				continue;

			struct item_data *card_data = itemdb->exists(equip->card[slot]);

			if (card_data != NULL && card_data->equip_script != NULL) {
				int idx;

				ARR_FIND(0, dis_items_cnt, idx, zone->disabled_items[idx] == card_data->nameid);

				if (idx == dis_items_cnt)
					script->run_item_equip_script(sd, card_data, npc->fake_nd->bl.id);
			}
		}
	}

	sd->npc_item_flag = iflag;

	return 1;
}

/**
 * Unequip an item at the given position.
 * @param sd the affected player structure. Must be checked before.
 * @param n inventory item position. Must be checked before.
 * @param pos slot position. Must be checked before.
 */
static void pc_unequipitem_pos(struct map_session_data *sd, int n, int pos)
{
	nullpo_retv(sd);
	if (pos & EQP_HAND_R) {
		sd->weapontype1 = W_FIST;
		pc->calcweapontype(sd);
		sd->status.look.weapon = 0;
		clif->changelook(&sd->bl, LOOK_WEAPON, sd->status.look.weapon);
		if (!battle_config.dancing_weaponswitch_fix)
			status_change_end(&sd->bl, SC_DANCING, INVALID_TIMER); // Unequipping => stop dancing.
	}
	if (pos & EQP_HAND_L) {
		sd->has_shield = false;
		sd->status.look.shield = 0;
		sd->weapontype2 = W_FIST;
		pc->calcweapontype(sd);
		clif->changelook(&sd->bl, LOOK_SHIELD, sd->status.look.shield);
	}
	if (pos & EQP_HEAD_LOW && pc->checkequip(sd,EQP_COSTUME_HEAD_LOW) == -1) {
		sd->status.look.head_bottom = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_BOTTOM, sd->status.look.head_bottom);
	}
	if (pos & EQP_HEAD_TOP && pc->checkequip(sd,EQP_COSTUME_HEAD_TOP) == -1) {
		sd->status.look.head_top = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_TOP, sd->status.look.head_top);
	}
	if (pos & EQP_HEAD_MID && pc->checkequip(sd,EQP_COSTUME_HEAD_MID) == -1) {
		sd->status.look.head_mid = 0;
		clif->changelook(&sd->bl, LOOK_HEAD_MID, sd->status.look.head_mid);
	}

	if (pos & EQP_COSTUME_HEAD_TOP) {
		int equip = pc->checkequip(sd, EQP_HEAD_TOP);
		sd->status.look.head_top = (equip >= 0 && sd->inventory_data[equip] != NULL) ? sd->inventory_data[equip]->view_sprite : 0;
		clif->changelook(&sd->bl, LOOK_HEAD_TOP, sd->status.look.head_top);
	}

	if (pos & EQP_COSTUME_HEAD_MID) {
		int equip = pc->checkequip(sd, EQP_HEAD_MID);
		sd->status.look.head_mid = (equip >= 0 && sd->inventory_data[equip] != NULL) ? sd->inventory_data[equip]->view_sprite : 0;
		clif->changelook(&sd->bl, LOOK_HEAD_MID, sd->status.look.head_mid);
	}

	if (pos & EQP_COSTUME_HEAD_LOW) {
		int equip = pc->checkequip(sd, EQP_HEAD_LOW);
		sd->status.look.head_bottom = (equip >= 0 && sd->inventory_data[equip] != NULL) ? sd->inventory_data[equip]->view_sprite : 0;
		clif->changelook(&sd->bl, LOOK_HEAD_BOTTOM, sd->status.look.head_bottom);
	}

	if (pos & EQP_SHOES)
		clif->changelook(&sd->bl,LOOK_SHOES,0);

	if (pos & EQP_GARMENT && pc->checkequip(sd,EQP_COSTUME_GARMENT) == -1) {
		sd->status.look.robe = 0;
		clif->changelook(&sd->bl, LOOK_ROBE, 0);
	}

	if (pos & EQP_COSTUME_GARMENT) {
		int equip = pc->checkequip(sd, EQP_GARMENT);
		sd->status.look.robe = (equip >= 0 && sd->inventory_data[equip] != NULL) ? sd->inventory_data[equip]->view_sprite : 0;
		clif->changelook(&sd->bl, LOOK_ROBE, sd->status.look.robe);
	}
}

/**
 * Attempts to unequip an item.
 *
 * @param sd The related character.
 * @param n The item's inventory index.
 * @param flag Modifier for additional actions. (See enum pc_unequipitem_flag.)
 * @return 0 on failure, 1 on success.
 *
 **/
static int pc_unequipitem(struct map_session_data *sd, int n, int flag)
{
	nullpo_ret(sd);

	if (n < 0 || n >= sd->status.inventorySize) {
		clif->unequipitemack(sd, 0, 0, UIA_FAIL);
		return 0;
	}

	// If the character is in berserk mode, the item can't be unequipped.
	if (sd->sc.count != 0 && (sd->sc.data[SC_BERSERK] != NULL || sd->sc.data[SC_NO_SWITCH_EQUIP] != NULL)
	    && (flag & PCUNEQUIPITEM_FORCE) == 0) {
		clif->unequipitemack(sd, n, 0, UIA_FAIL);
		return 0;
	}

	if ((flag & PCUNEQUIPITEM_FORCE) == 0 && sd->sc.count != 0 && sd->sc.data[SC_KYOUGAKU] != NULL) {
		clif->unequipitemack(sd, n, 0, UIA_FAIL);
		return 0;
	}

	if (battle_config.battle_log != 0)
		ShowInfo("unequip %d %x:%x\n", n, (unsigned int)(pc->equippoint(sd, n)), sd->status.inventory[n].equip);

	if (sd->status.inventory[n].equip == 0) { // Nothing to unequip.
		clif->unequipitemack(sd, n, 0, UIA_FAIL);
		return 0;
	}

	for (int i = 0; i < EQI_MAX; i++) {
		if ((sd->status.inventory[n].equip & pc->equip_pos[i]) != 0)
			sd->equip_index[i] = -1;
	}

	int pos = sd->status.inventory[n].equip;

	pc->unequipitem_pos(sd, n, pos);
	clif->unequipitemack(sd, n, pos, UIA_SUCCESS);

	status_change_end(&sd->bl, SC_HEAT_BARREL, INVALID_TIMER);
	if ((pos & EQP_ARMS) != 0 && sd->weapontype1 == W_FIST && sd->weapontype2 == W_FIST
	    && (sd->sc.data[SC_TK_SEVENWIND] == NULL || sd->sc.data[SC_ASPERSIO] != NULL)) { // Check for Seven Wind. (But not level seven!)
		skill->enchant_elemental_end(&sd->bl, -1);
	}

	if ((pos & EQP_ARMOR) != 0) {
		status_change_end(&sd->bl, SC_BENEDICTIO, INVALID_TIMER);
		status_change_end(&sd->bl, SC_ARMOR_RESIST, INVALID_TIMER);
	}

#ifdef RENEWAL
	if (battle->bc->bow_unequip_arrow != 0 && (pos & EQP_ARMS) != 0 && sd->equip_index[EQI_AMMO] > 0)
		pc->unequipitem(sd, sd->equip_index[EQI_AMMO], PCUNEQUIPITEM_FORCE);
#endif

	if (sd->inventory_data[n] != NULL) {
		if (sd->inventory_data[n]->type == IT_AMMO && (sd->inventory_data[n]->nameid != ITEMID_SILVER_BULLET || sd->inventory_data[n]->nameid != ITEMID_SANCTIFIED_BULLET || sd->inventory_data[n]->nameid != ITEMID_SILVER_BULLET_))
			status_change_end(&sd->bl, SC_PLATINUM_ALTER, INVALID_TIMER);
	}

	if ((sd->state.autobonus & pos) != 0)  // Check for activated autobonus. [Inkfish]
		sd->state.autobonus &= ~sd->status.inventory[n].equip;

	sd->status.inventory[n].equip = 0;

	bool status_calc = false;
	int iflag = sd->npc_item_flag;

	// Check for combos. (MUST be done before status->calc_pc()!)
	if (sd->inventory_data[n] != NULL) {
		if (sd->inventory_data[n]->combos_count != 0 && pc->removecombo(sd, sd->inventory_data[n]) != 0)
			status_calc = true;

		if (!itemdb_isspecial(sd->status.inventory[n].card[0])) {
			for (int i = 0; i < sd->inventory_data[n]->slot; i++) {
				if (sd->status.inventory[n].card[i] == 0)
					continue;

				struct item_data *data = itemdb->exists(sd->status.inventory[n].card[i]);

				if (data != NULL && data->combos_count != 0 && pc->removecombo(sd, data) != 0)
					status_calc = true;
			}
		}

		// Check item options.
		for (int i = 0; i < MAX_ITEM_OPTIONS; i++) {
			if (sd->status.inventory[n].option[i].index <= 0)
				continue;

			if (itemdb->option_exists(sd->status.inventory[n].option[i].index) == NULL)
				continue;

			status_calc = true;
		}
	}

	if ((flag & PCUNEQUIPITEM_RECALC) != 0 || status_calc) {
		pc->checkallowskill(sd);
		status_calc_pc(sd, SCO_NONE);
	}

	if (sd->sc.data[SC_CRUCIS] != NULL && !battle->check_undead(sd->battle_status.race, sd->battle_status.def_ele))
		status_change_end(&sd->bl, SC_CRUCIS, INVALID_TIMER);

	// Execute unequip script. [Skotlex]
	if (sd->inventory_data[n] != NULL) {
		struct item_data *equip_data = sd->inventory_data[n];
		struct map_zone_data *zone = map->list[sd->bl.m].zone;
		int dis_items_cnt = zone->disabled_items_count;

		if (equip_data->unequip_script != NULL) {
			int idx;

			ARR_FIND(0, dis_items_cnt, idx, zone->disabled_items[idx] == equip_data->nameid);

			if (idx == dis_items_cnt)
				script->run_item_unequip_script(sd, equip_data, npc->fake_nd->bl.id);
		}

		struct item *equip = &sd->status.inventory[n];

		if (!itemdb_isspecial(equip->card[0])) {
			for (int slot = 0; slot < equip_data->slot; slot++) {
				if (equip->card[slot] == 0)
					continue;

				struct item_data *card_data = itemdb->exists(equip->card[slot]);

				if (card_data != NULL && card_data->unequip_script != NULL) {
					int idx;

					ARR_FIND(0, dis_items_cnt, idx, zone->disabled_items[idx] == card_data->nameid);

					if (idx == dis_items_cnt)
						script->run_item_unequip_script(sd, card_data, npc->fake_nd->bl.id);
				}
			}
		}
	}

	sd->npc_item_flag = iflag;

	return 1;
}

/*==========================================
 * Checking if player (sd) have unauthorize, invalide item
 * on inventory, cart, equiped for the map (item_noequip)
 *------------------------------------------*/
static int pc_checkitem(struct map_session_data *sd)
{
	int i, calc_flag = 0;

	nullpo_ret(sd);

	if (sd->state.vending == 1) // Avoid reorganizing items when we are vending, as that leads to exploits (pointed out by End of Exam)
		return 0;

	if (sd->itemcheck != PCCHECKITEM_NONE) { // check for invalid(ated) items
		int id = 0;

		if (sd->itemcheck & PCCHECKITEM_INVENTORY) {
			for (i = 0; i < sd->status.inventorySize; i++) {
				if ((id = sd->status.inventory[i].nameid) == 0)
					continue;

				if (!itemdb_available(id)) {
					ShowWarning("pc_checkitem: Removed invalid/disabled item id %d from inventory (amount=%d, char_id=%d).\n", id, sd->status.inventory[i].amount, sd->status.char_id);
					pc->delitem(sd, i, sd->status.inventory[i].amount, 0, DELITEM_NORMAL, LOG_TYPE_INV_INVALID);
					continue;
				}

				if (sd->status.inventory[i].unique_id == 0 && !itemdb->isstackable(id))
					sd->status.inventory[i].unique_id = itemdb->unique_id(sd);
			}

			sd->itemcheck &= ~PCCHECKITEM_INVENTORY;
		}

		if (sd->itemcheck & PCCHECKITEM_CART) {
			for (i = 0; i < MAX_CART; i++) {
				if ((id = sd->status.cart[i].nameid) == 0)
					continue;

				if( !itemdb_available(id) ) {
					ShowWarning("pc_checkitem: Removed invalid/disabled item id %d from cart (amount=%d, char_id=%d).\n", id, sd->status.cart[i].amount, sd->status.char_id);
					pc->cart_delitem(sd, i, sd->status.cart[i].amount, 0, LOG_TYPE_CART_INVALID);
					continue;
				}

				if (sd->status.cart[i].unique_id == 0 && !itemdb->isstackable(id))
					sd->status.cart[i].unique_id = itemdb->unique_id(sd);
			}

			sd->itemcheck &= ~PCCHECKITEM_CART;
		}

		if (sd->itemcheck & PCCHECKITEM_STORAGE && sd->storage.received == true) {
			for (i = 0; i < VECTOR_LENGTH(sd->storage.item); i++) {
				struct item *it = &VECTOR_INDEX(sd->storage.item, i);

				if ((id = it->nameid) == 0)
					continue;

				if (!itemdb_available(id)) {
					ShowWarning("pc_checkitem: Removed invalid/disabled item id %d from storage (amount=%d, char_id=%d).\n", id, it->amount, sd->status.char_id);
					storage->delitem(sd, i, it->amount);
					continue;
				}

				if (it->unique_id == 0 && itemdb->isstackable(id) == 0)
					it->unique_id = itemdb->unique_id(sd);
			}

			storage->close(sd);

			sd->itemcheck &= ~PCCHECKITEM_STORAGE;
		}

		if (sd->guild && sd->itemcheck & PCCHECKITEM_GSTORAGE) {
			struct guild_storage *guild_storage = idb_get(gstorage->db,sd->guild->guild_id);
			if (guild_storage) {
				for (i = 0; i < guild_storage->items.capacity; i++) {
					if ((id = guild_storage->items.data[i].nameid) == 0)
						continue;

					if (!itemdb_available(id)) {
						ShowWarning("pc_checkitem: Removed invalid/disabled item id %d from guild storage (amount=%d, char_id=%d, guild_id=%d).\n",
								id, guild_storage->items.data[i].amount, sd->status.char_id, sd->guild->guild_id);
						gstorage->delitem(sd, guild_storage, i, guild_storage->items.data[i].amount);
						gstorage->close(sd); // force closing
						continue;
					}

					if (guild_storage->items.data[i].unique_id == 0 && !itemdb->isstackable(id))
						guild_storage->items.data[i].unique_id = itemdb->unique_id(sd);
				}
			}

			sd->itemcheck &= ~PCCHECKITEM_GSTORAGE;
		}
	}

	for (i = 0; i < sd->status.inventorySize; i++) {

		if (sd->status.inventory[i].nameid == 0)
			continue;

		if (sd->status.inventory[i].equip == 0)
			continue;

		if (sd->status.inventory[i].equip & ~pc->equippoint(sd,i)) {
			pc->unequipitem(sd, i, PCUNEQUIPITEM_FORCE);
			calc_flag = 1;
			continue;
		}

		if (battle_config.unequip_restricted_equipment & 1) {
			int j;
			for (j = 0; j < map->list[sd->bl.m].zone->disabled_items_count; j++) {
				if (map->list[sd->bl.m].zone->disabled_items[j] == sd->status.inventory[i].nameid) {
					pc->unequipitem(sd, i, PCUNEQUIPITEM_FORCE);
					calc_flag = 1;
				}
			}
		}

		if (battle_config.unequip_restricted_equipment & 2) {
			if (!itemdb_isspecial(sd->status.inventory[i].card[0])) {
				int j, slot;
				for (slot = 0; slot < MAX_SLOTS; slot++) {
					for (j = 0; j < map->list[sd->bl.m].zone->disabled_items_count; j++) {
						if (map->list[sd->bl.m].zone->disabled_items[j] == sd->status.inventory[i].card[slot]) {
							pc->unequipitem(sd, i, PCUNEQUIPITEM_FORCE);
							calc_flag = 1;
						}
					}
				}
			}
		}

	}

	if (calc_flag != 0 && sd->state.active == 1) {
		pc->checkallowskill(sd);
		status_calc_pc(sd, SCO_NONE);
	}

	return 0;
}

/*==========================================
 * Update PVP rank for sd1 in cmp to sd2
 *------------------------------------------*/
static int pc_calc_pvprank_sub(struct block_list *bl, va_list ap)
{
	struct map_session_data *sd1 = NULL;
	struct map_session_data *sd2 = va_arg(ap,struct map_session_data *);

	nullpo_ret(bl);
	Assert_ret(bl->type == BL_PC);
	sd1 = BL_UCAST(BL_PC, bl);
	nullpo_ret(sd2);

	if (pc_isinvisible(sd1) ||pc_isinvisible(sd2)) {
		// cannot register pvp rank for hidden GMs
		return 0;
	}

	if( sd1->pvp_point > sd2->pvp_point )
		sd2->pvp_rank++;
	return 0;
}
/*==========================================
 * Calculate new rank beetween all present players (map->foreachinarea)
 * and display result
 *------------------------------------------*/
static int pc_calc_pvprank(struct map_session_data *sd)
{
	nullpo_ret(sd);
	struct map_data *m = &map->list[sd->bl.m];
	int old = sd->pvp_rank;
	sd->pvp_rank = 1;
	map->foreachinmap(pc->calc_pvprank_sub, sd->bl.m, BL_PC, sd);
	if (old != sd->pvp_rank || sd->pvp_lastusers != m->users_pvp) {
		sd->pvp_lastusers = m->users_pvp;
		clif->pvpset(sd, sd->pvp_rank, sd->pvp_lastusers, 0);
	}
	return sd->pvp_rank;
}
/*==========================================
 * Calculate next sd ranking calculation from config
 *------------------------------------------*/
static int pc_calc_pvprank_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd;

	sd=map->id2sd(id);
	if(sd==NULL)
		return 0;
	sd->pvp_timer = INVALID_TIMER;

	if (pc_isinvisible(sd)) {
		// do not calculate the pvp rank for a hidden GM
		return 0;
	}

	if( pc->calc_pvprank(sd) > 0 )
		sd->pvp_timer = timer->add(timer->gettick()+PVP_CALCRANK_INTERVAL,pc->calc_pvprank_timer,id,data);
	return 0;
}

/*==========================================
 * Checking if sd is married
 * Return:
 *   partner_id = yes
 *   0 = no
 *------------------------------------------*/
static int pc_ismarried(struct map_session_data *sd)
{
	if(sd == NULL)
		return -1;
	if(sd->status.partner_id > 0)
		return sd->status.partner_id;
	else
		return 0;
}
/*==========================================
 * Marry player sd to player dstsd
 * Return:
 *   -1 = fail
 *   0 = success
 *------------------------------------------*/
static int pc_marriage(struct map_session_data *sd, struct map_session_data *dstsd)
{
	if(sd == NULL || dstsd == NULL ||
		sd->status.partner_id > 0 || dstsd->status.partner_id > 0 ||
		(sd->job & JOBL_BABY) != 0 || (dstsd->job & JOBL_BABY) != 0)
		return -1;
	sd->status.partner_id = dstsd->status.char_id;
	dstsd->status.partner_id = sd->status.char_id;

	// Achievements [Smokexyz/Hercules]
	achievement->validate_marry(sd);
	achievement->validate_marry(dstsd);

	return 0;
}

/*==========================================
 * Divorce sd from its partner
 * Return:
 *   -1 = fail
 *   0 = success
 *------------------------------------------*/
static int pc_divorce(struct map_session_data *sd)
{
	struct map_session_data *p_sd;
	int i;

	if( sd == NULL || !pc->ismarried(sd) )
		return -1;

	if( !sd->status.partner_id )
		return -1; // Char is not married

	if( (p_sd = map->charid2sd(sd->status.partner_id)) == NULL ) {
		// Lets char server do the divorce
		if( chrif->divorce(sd->status.char_id, sd->status.partner_id) )
			return -1; // No char server connected

		return 0;
	}

	// Both players online, lets do the divorce manually
	sd->status.partner_id = 0;
	p_sd->status.partner_id = 0;
	for (i = 0; i < sd->status.inventorySize; i++)
	{
		if (sd->status.inventory[i].nameid == WEDDING_RING_M || sd->status.inventory[i].nameid == WEDDING_RING_F)
			pc->delitem(sd, i, 1, 0, DELITEM_NORMAL, LOG_TYPE_DIVORCE);
	}
	for (i = 0; i < p_sd->status.inventorySize; i++)
	{
		if (p_sd->status.inventory[i].nameid == WEDDING_RING_M || p_sd->status.inventory[i].nameid == WEDDING_RING_F)
			pc->delitem(p_sd, i, 1, 0, DELITEM_NORMAL, LOG_TYPE_DIVORCE);
	}

	clif->divorced(sd, p_sd->status.name);
	clif->divorced(p_sd, sd->status.name);

	return 0;
}

/*==========================================
 * Get sd partner charid. (Married partner)
 *------------------------------------------*/
static struct map_session_data *pc_get_partner(struct map_session_data *sd)
{
	if (sd && pc->ismarried(sd))
		// charid2sd returns NULL if not found
		return map->charid2sd(sd->status.partner_id);

	return NULL;
}

/*==========================================
 * Get sd father charid. (Need to be baby)
 *------------------------------------------*/
static struct map_session_data *pc_get_father(struct map_session_data *sd)
{
	if (sd && (sd->job & JOBL_BABY) != 0 && sd->status.father > 0)
		// charid2sd returns NULL if not found
		return map->charid2sd(sd->status.father);

	return NULL;
}

/*==========================================
 * Get sd mother charid. (Need to be baby)
 *------------------------------------------*/
static struct map_session_data *pc_get_mother(struct map_session_data *sd)
{
	if (sd && (sd->job & JOBL_BABY) != 0 && sd->status.mother > 0)
		// charid2sd returns NULL if not found
		return map->charid2sd(sd->status.mother);

	return NULL;
}

/*==========================================
 * Get sd children charid. (Need to be married)
 *------------------------------------------*/
static struct map_session_data *pc_get_child(struct map_session_data *sd)
{
	if (sd && pc->ismarried(sd) && sd->status.child > 0)
		// charid2sd returns NULL if not found
		return map->charid2sd(sd->status.child);

	return NULL;
}

/*==========================================
 * Set player sd to bleed. (losing hp and/or sp each diff_tick)
 *------------------------------------------*/
static void pc_bleeding(struct map_session_data *sd, unsigned int diff_tick)
{
	int hp = 0, sp = 0;

	nullpo_retv(sd);
	if( pc_isdead(sd) )
		return;

	if (sd->hp_loss.value) {
		sd->hp_loss.tick += diff_tick;
		while (sd->hp_loss.tick >= sd->hp_loss.rate) {
			hp += sd->hp_loss.value;
			sd->hp_loss.tick -= sd->hp_loss.rate;
		}
		if(hp >= sd->battle_status.hp)
			hp = sd->battle_status.hp-1; //Script drains cannot kill you.
	}

	if (sd->sp_loss.value) {
		sd->sp_loss.tick += diff_tick;
		while (sd->sp_loss.tick >= sd->sp_loss.rate) {
			sp += sd->sp_loss.value;
			sd->sp_loss.tick -= sd->sp_loss.rate;
		}
	}

	if (hp > 0 || sp > 0)
		status_zap(&sd->bl, hp, sp);

	return;
}

//Character regen. Flag is used to know which types of regen can take place.
//&1: HP regen
//&2: SP regen
static void pc_regen(struct map_session_data *sd, unsigned int diff_tick)
{
	int hp = 0, sp = 0;

	nullpo_retv(sd);
	if (sd->hp_regen.value) {
		sd->hp_regen.tick += diff_tick;
		while (sd->hp_regen.tick >= sd->hp_regen.rate) {
			hp += sd->hp_regen.value;
			sd->hp_regen.tick -= sd->hp_regen.rate;
		}
	}

	if (sd->sp_regen.value) {
		sd->sp_regen.tick += diff_tick;
		while (sd->sp_regen.tick >= sd->sp_regen.rate) {
			sp += sd->sp_regen.value;
			sd->sp_regen.tick -= sd->sp_regen.rate;
		}
	}

	if (hp > 0 || sp > 0)
		status->heal(&sd->bl, hp, sp, STATUS_HEAL_DEFAULT);

	return;
}

/*==========================================
 * Memo player sd savepoint. (map,x,y)
 *------------------------------------------*/
static int pc_setsavepoint(struct map_session_data *sd, short map_index, int x, int y)
{
	nullpo_ret(sd);

	sd->status.save_point.map = map_index;
	sd->status.save_point.x = x;
	sd->status.save_point.y = y;

	return 0;
}

/*==========================================
 * Save 1 player data at autosave intervall
 *------------------------------------------*/
static int pc_autosave(int tid, int64 tick, int id, intptr_t data)
{
	int interval;
	struct s_mapiterator* iter;
	struct map_session_data* sd;
	static int last_save_id = 0, save_flag = 0;

	if(save_flag == 2) //Someone was saved on last call, normal cycle
		save_flag = 0;
	else
		save_flag = 1; //Noone was saved, so save first found char.

	iter = mapit_getallusers();
	for (sd = BL_UCAST(BL_PC, mapit->first(iter)); mapit->exists(iter); sd = BL_UCAST(BL_PC, mapit->next(iter))) {
		if(sd->bl.id == last_save_id && save_flag != 1) {
			save_flag = 1;
			continue;
		}

		if(save_flag != 1) //Not our turn to save yet.
			continue;

		//Save char.
		last_save_id = sd->bl.id;
		save_flag = 2;

		chrif->save(sd,0);
		break;
	}
	mapit->free(iter);

	interval = map->autosave_interval/(map->usercount()+1);
	if(interval < map->minsave_interval)
		interval = map->minsave_interval;
	timer->add(timer->gettick()+interval,pc->autosave,0,0);

	return 0;
}

static int pc_daynight_timer_sub(struct map_session_data *sd, va_list ap)
{
	nullpo_ret(sd);
	if (sd->state.night != map->night_flag && map->list[sd->bl.m].flag.nightenabled) { //Night/day state does not match.
		clif->status_change(&sd->bl, status->get_sc_icon(SC_SKE), status->get_sc_relevant_bl_types(SC_SKE), map->night_flag, 0, 0, 0, 0); //New night effect by dynamix [Skotlex]
		sd->state.night = map->night_flag;
		return 1;
	}
	return 0;
}
/*================================================
 * timer to do the day [Yor]
 * data: 0 = called by timer, 1 = gmcommand/script
 *------------------------------------------------*/
static int map_day_timer(int tid, int64 tick, int id, intptr_t data)
{
	char tmp_soutput[1024];

	if (data == 0 && battle_config.day_duration <= 0) // if we want a day
		return 0;

	if (!map->night_flag)
		return 0; //Already day.

	map->night_flag = 0; // 0=day, 1=night [Yor]
	map->foreachpc(pc->daynight_timer_sub);
	safestrncpy(tmp_soutput, (data == 0) ? msg_txt(502) : msg_txt(60), sizeof(tmp_soutput)); // The day has arrived!
	clif->broadcast(NULL, tmp_soutput, (int)strlen(tmp_soutput) + 1, BC_DEFAULT, ALL_CLIENT);
	return 0;
}

/*================================================
 * timer to do the night [Yor]
 * data: 0 = called by timer, 1 = gmcommand/script
 *------------------------------------------------*/
static int map_night_timer(int tid, int64 tick, int id, intptr_t data)
{
	char tmp_soutput[1024];

	if (data == 0 && battle_config.night_duration <= 0) // if we want a night
		return 0;

	if (map->night_flag)
		return 0; //Already nigth.

	map->night_flag = 1; // 0=day, 1=night [Yor]
	map->foreachpc(pc->daynight_timer_sub);
	safestrncpy(tmp_soutput, (data == 0) ? msg_txt(503) : msg_txt(59), sizeof(tmp_soutput)); // The night has fallen...
	clif->broadcast(NULL, tmp_soutput, (int)strlen(tmp_soutput) + 1, BC_DEFAULT, ALL_CLIENT);
	return 0;
}

static void pc_setstand(struct map_session_data *sd)
{
	nullpo_retv(sd);

	status_change_end(&sd->bl, SC_TENSIONRELAX, INVALID_TIMER);
	clif->sc_end(&sd->bl, sd->bl.id, SELF, status->get_sc_icon(SC_SIT));
	//Reset sitting tick.
	sd->sitting_regen.tick.hp = 0;
	sd->sitting_regen.tick.sp = 0;
	if (pc_isdead(sd)) {
		sd->state.dead_sit = sd->vd.dead_sit = 0;
		clif->party_dead_notification(sd);
	} else {
		sd->state.dead_sit = sd->vd.dead_sit = 0;
	}
}

/**
 * Mechanic (MADO GEAR)
 **/
static void pc_overheat(struct map_session_data *sd, int val)
{
	int heat = val, skill_lv,
		limit[] = { 10, 20, 28, 46, 66 };

	nullpo_retv(sd);
	if( !pc_ismadogear(sd) || sd->sc.data[SC_OVERHEAT] )
		return; // already burning

	skill_lv = cap_value(pc->checkskill(sd,NC_MAINFRAME),0,4);
	if( sd->sc.data[SC_OVERHEAT_LIMITPOINT] ) {
		heat += sd->sc.data[SC_OVERHEAT_LIMITPOINT]->val1;
		status_change_end(&sd->bl,SC_OVERHEAT_LIMITPOINT,INVALID_TIMER);
	}

	heat = max(0,heat); // Avoid negative HEAT
	if( heat >= limit[skill_lv] )
		sc_start(NULL, &sd->bl, SC_OVERHEAT, 100, 0, 1000, 0);
	else
		sc_start(NULL, &sd->bl, SC_OVERHEAT_LIMITPOINT, 100, heat, 30000, 0);

	return;
}

/**
 * Check if player is autolooting given itemID.
 */
static bool pc_isautolooting(struct map_session_data *sd, int nameid)
{
	int i = 0;

	nullpo_ret(sd);
	if (sd->state.autoloottype && sd->state.autoloottype&(1<<itemdb_type(nameid)))
		return true;

	if (!sd->state.autolooting)
		return false;

	ARR_FIND(0, AUTOLOOTITEM_SIZE, i, sd->state.autolootid[i] == nameid);

	return (i != AUTOLOOTITEM_SIZE);
}

/**
 * Checks if player can use @/#command
 * @param sd Player map session data
 * @param command Command name with @/# and without params
 */
static bool pc_can_use_command(struct map_session_data *sd, const char *command)
{
	return atcommand->can_use(sd,command);
}

/**
 * Spirit Charm expiration timer.
 *
 * @see TimerFunc
 */
static int pc_charm_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);
	int i;

	if (sd == NULL)
		return 1;

	if (sd->charm_count <= 0) {
		ShowError("pc_charm_timer: %d spiritcharm's available. (aid=%d cid=%d tid=%d)\n", sd->charm_count, sd->status.account_id, sd->status.char_id, tid);
		sd->charm_count = 0;
		sd->charm_type = CHARM_TYPE_NONE;
		return 0;
	}

	ARR_FIND(0, sd->charm_count, i, sd->charm_timer[i] == tid);
	if (i == sd->charm_count) {
		ShowError("pc_charm_timer: timer not found (aid=%d cid=%d tid=%d)\n", sd->status.account_id, sd->status.char_id, tid);
		return 0;
	}

	sd->charm_count--;
	if(i != sd->charm_count)
		memmove(sd->charm_timer+i, sd->charm_timer+i+1, (sd->charm_count-i)*sizeof(int));
	sd->charm_timer[sd->charm_count] = INVALID_TIMER;
	if (sd->charm_count <= 0)
		sd->charm_type = CHARM_TYPE_NONE;

	clif->spiritcharm(sd);

	return 0;
}

/**
 * Adds a spirit charm.
 *
 * @param sd       Target character.
 * @param interval Duration.
 * @param max      Maximum amount of charms to add.
 * @param type     Charm type (@see spirit_charm_types)
 */
static void pc_add_charm(struct map_session_data *sd, int interval, int max, enum spirit_charm_types type)
{
	int tid, i;

	nullpo_retv(sd);

	if (sd->charm_type != CHARM_TYPE_NONE && type != sd->charm_type) {
		pc->del_charm(sd, sd->charm_count, sd->charm_type);
	}

	if (max > MAX_SPIRITCHARM)
		max = MAX_SPIRITCHARM;
	if (sd->charm_count < 0)
		sd->charm_count = 0;

	if (sd->charm_count && sd->charm_count >= max) {
		if (sd->charm_timer[0] != INVALID_TIMER)
			timer->delete(sd->charm_timer[0],pc->charm_timer);
		sd->charm_count--;
		if (sd->charm_count != 0)
			memmove(sd->charm_timer+0, sd->charm_timer+1, sd->charm_count*sizeof(int));
		sd->charm_timer[sd->charm_count] = INVALID_TIMER;
	}

	tid = timer->add(timer->gettick()+interval, pc->charm_timer, sd->bl.id, 0);
	ARR_FIND(0, sd->charm_count, i, sd->charm_timer[i] == INVALID_TIMER || DIFF_TICK(timer->get(tid)->tick, timer->get(sd->charm_timer[i])->tick) < 0);
	if (i != sd->charm_count)
		memmove(sd->charm_timer+i+1, sd->charm_timer+i, (sd->charm_count-i)*sizeof(int));
	sd->charm_timer[i] = tid;
	sd->charm_count++;
	sd->charm_type = type;

	clif->spiritcharm(sd);
}

/**
 * Removes one or more spirit charms.
 *
 * @param sd    The target character.
 * @param count Amount of charms to remove.
 * @param type  Type of charm to remove.
 */
static void pc_del_charm(struct map_session_data *sd, int count, enum spirit_charm_types type)
{
	int i;

	nullpo_retv(sd);

	if (sd->charm_type != type)
		return;

	if (sd->charm_count <= 0) {
		sd->charm_count = 0;
		return;
	}

	if (count <= 0)
		return;
	if (count > sd->charm_count)
		count = sd->charm_count;
	sd->charm_count -= count;
	if (count > MAX_SPIRITCHARM)
		count = MAX_SPIRITCHARM;

	for (i = 0; i < count; i++) {
		if(sd->charm_timer[i] != INVALID_TIMER) {
			timer->delete(sd->charm_timer[i],pc->charm_timer);
			sd->charm_timer[i] = INVALID_TIMER;
		}
	}
	for (i = count; i < MAX_SPIRITCHARM; i++) {
		sd->charm_timer[i-count] = sd->charm_timer[i];
		sd->charm_timer[i] = INVALID_TIMER;
	}
	if (sd->charm_count <= 0)
		sd->charm_type = CHARM_TYPE_NONE;

	clif->spiritcharm(sd);
}

/**
 * Renewal EXP/Itemdrop rate modifier base on level penalty.
 *
 * @param diff Level difference.
 * @param race Monster race.
 * @param mode Monster mode.
 * @param type Modifier type (1=exp 2=itemdrop)
 * @return The percent rate modifier (100 = 100%)
 */
static int pc_level_penalty_mod(int diff, unsigned char race, uint32 mode, int type)
{
#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
	int rate = 100, i;

	if( diff < 0 )
		diff = MAX_LEVEL + ( ~diff + 1 );

	for (i = RC_FORMLESS; i < RC_MAX; i++) {
		int tmp;

		if (race != i) {
			if (mode&MD_BOSS && i < RC_BOSS)
				i = RC_BOSS;
			else if (i <= RC_BOSS)
				continue;
		}

		if ((tmp=pc->level_penalty[type][i][diff]) > 0) {
			rate = tmp;
			break;
		}
	}

	return rate;
#else
	return 100;
#endif
}

static bool pc_read_skill_job_skip(short skill_id, int job_id)
{
	return skill_id == NV_TRICKDEAD && ((pc->jobid2mapid(job_id) & (MAPID_BASEMASK | JOBL_2)) != MAPID_NOVICE);  // skip trickdead for non-novices
}

/**
 * Parses the skill tree config file.
 *
 * In order to reclaim the memory allocated by this function
 * `pc->clear_skill_tree()` should be used.
 *
 * @remark
 *   This function assumes that the skill tree is clear and zeroed.
 *   If it has been already loaded (ie reloading), it needs to be cleared
 *   before calling this function again.
 *
 * @author [Ind/Hercules]
 */
static void pc_read_skill_tree(void)
{
	struct config_t skill_tree_conf;
	struct config_setting_t *skt = NULL;
	char config_filename[280];
	int i = 0;
	struct s_mapiterator *iter;
	struct map_session_data *sd;
	bool loaded[CLASS_COUNT] = { false };

	snprintf(config_filename, sizeof(config_filename), "%s/"DBPATH"skill_tree.conf", map->db_path);
	if (!libconfig->load_file(&skill_tree_conf, config_filename))
		return;

	// Foreach job
	while ((skt = libconfig->setting_get_elem(skill_tree_conf.root, i++))) {
		struct config_setting_t *t = NULL;
		int job_idx;
		const char *job_name = config_setting_name(skt);
		int job_id = pc->check_job_name(job_name);

		if (job_id == -1) {
			ShowWarning("pc_read_skill_tree: '%s' unknown job name!\n", job_name);
			continue;
		}
		job_idx = pc->class2idx(job_id);
		if (loaded[job_idx]) {
			ShowWarning("pc_read_skill_tree: Duplicate entry for job '%s'. Skipping.\n", job_name);
			continue;
		}
		loaded[job_idx] = true;

		if ((t = libconfig->setting_get_member(skt, "inherit")) != NULL) {
			int j = 0;
			const char *ijob_name = NULL;
			// Foreach inherited job
			while ((ijob_name = libconfig->setting_get_string_elem(t, j++)) != NULL) {
				int k, ijob_idx;
				int ijob_id = pc->check_job_name(ijob_name);

				if (ijob_id == -1) {
					ShowWarning("pc_read_skill_tree: '%s' trying to inherit unknown '%s'!\n", job_name, ijob_name);
					continue;
				}
				ijob_idx = pc->class2idx(ijob_id);
				if (ijob_idx == job_idx) {
					ShowWarning("pc_read_skill_tree: '%s' trying to inherit itself. Skipping.\n", job_name);
					continue;
				}
				if (!loaded[ijob_idx]) {
					ShowWarning("pc_read_skill_tree: '%s' trying to inherit not yet loaded '%s' (wrong order in the tree). Skipping.\n", job_name, ijob_name);
					continue;
				}

				for (k = 0; k < MAX_SKILL_TREE; k++) {
					int cur;
					struct skill_tree_entry *dst = NULL;
					const struct skill_tree_entry *src = &pc->skill_tree[ijob_idx][k];

					if (src->id == 0)
						break; // No more skills to copy

					ARR_FIND(0, MAX_SKILL_TREE, cur, pc->skill_tree[job_idx][cur].id == 0 || pc->skill_tree[job_idx][cur].id == src->id);
					if (cur == MAX_SKILL_TREE) {
						ShowWarning("pc_read_skill_tree: '%s' can't inherit '%s', skill tree is full!\n", job_name, ijob_name);
						break;
					}
					if (pc->read_skill_job_skip(src->id, job_id))
						continue;
					dst = &pc->skill_tree[job_idx][cur];
					dst->inherited = 1;
					if (dst->id == 0) {
						// Not existing yet, copy
						dst->id = src->id;
						dst->idx = src->idx;
						dst->max = src->max;
						dst->joblv = src->joblv;
						VECTOR_INIT(dst->need);
						if (VECTOR_LENGTH(src->need) > 0) {
							VECTOR_ENSURE(dst->need, VECTOR_LENGTH(src->need), 1);
							VECTOR_PUSHARRAY(dst->need, VECTOR_DATA(src->need), VECTOR_LENGTH(src->need));
						}
					} else {
						int l;
						// Already existing, merge
						if (src->max > dst->max)
							dst->max = src->max;
						dst->joblv = src->joblv;
						for (l = 0; l < VECTOR_LENGTH(src->need); l++) {
							int m;
							struct skill_tree_requirement *sreq = &VECTOR_INDEX(src->need, l);
							ARR_FIND(0, VECTOR_LENGTH(dst->need), m, VECTOR_INDEX(dst->need, m).id == sreq->id);
							if (m == VECTOR_LENGTH(dst->need)) {
								VECTOR_ENSURE(dst->need, 1, 1);
								VECTOR_PUSHCOPY(dst->need, sreq);
							} else {
								struct skill_tree_requirement *dreq = &VECTOR_INDEX(dst->need, m);
								dreq->lv = sreq->lv;
							}
						}
					}
				}
			}
		}
		if ((t = libconfig->setting_get_member(skt, "skills")) != NULL) {
			int j = 0;
			struct config_setting_t *sk = NULL;
			// Foreach skill
			while ((sk = libconfig->setting_get_elem(t, j++)) != NULL) {
				int skill_id, sk_idx;
				struct config_setting_t *rsk = NULL;
				const char *sk_name = config_setting_name(sk);
				struct skill_tree_entry *tree_entry = NULL;

				if ((skill_id = skill->name2id(sk_name)) == 0) {
					ShowWarning("pc_read_skill_tree: unknown skill '%s' in '%s'\n", sk_name, job_name);
					continue;
				}

				ARR_FIND(0, MAX_SKILL_TREE, sk_idx, pc->skill_tree[job_idx][sk_idx].id == 0 || pc->skill_tree[job_idx][sk_idx].id == skill_id);
				if (sk_idx == MAX_SKILL_TREE) {
					ShowWarning("pc_read_skill_tree: Unable to load skill %d (%s) into '%s's tree. Maximum number of skills per class has been reached.\n", skill_id, sk_name, job_name);
					continue;
				}
				tree_entry = &pc->skill_tree[job_idx][sk_idx];

				if (tree_entry->id != 0 && !tree_entry->inherited) {
					ShowNotice("pc_read_skill_tree: Duplicate %d for '%s' (%d). Skipping.\n", skill_id, job_name, job_id);
					continue;
				}
				if (config_setting_is_group(sk)) {
					int i32 = 0;
					if (libconfig->setting_lookup_int(sk, "MaxLevel", &i32) && i32 > 0) {
						tree_entry->max = (unsigned char)i32;
					} else {
						ShowWarning("pc_read_skill_tree: missing MaxLevel for skill %d (%s) class '%s'. Skipping.\n", skill_id, sk_name, job_name);
						continue;
					}
					if (libconfig->setting_lookup_int(sk, "MinJobLevel", &i32) && i32 > 0) {
						tree_entry->joblv = (unsigned char)i32;
					} else if (!tree_entry->inherited) {
						tree_entry->joblv = 0;
					}
				} else {
					tree_entry->max = (unsigned char)libconfig->setting_get_int(sk);
					if (!tree_entry->inherited)
						tree_entry->joblv = 0;
				}
				if (!tree_entry->inherited) {
					tree_entry->id  = skill_id;
					tree_entry->idx = skill->get_index(skill_id);
					VECTOR_INIT(tree_entry->need);
				}

				if (config_setting_is_group(sk)) {
					int k = 0;
					// Foreach requirement
					while ((rsk = libconfig->setting_get_elem(sk, k++)) != NULL) {
						const char *rsk_name = config_setting_name(rsk);
						int rsk_id = skill->name2id(rsk_name);
						struct skill_tree_requirement *req = NULL;
						int l;

						if (rsk_id == 0) {
							if (strcmp(rsk_name, "MaxLevel") != 0 && strcmp(rsk_name, "MinJobLevel") != 0)
								ShowWarning("pc_read_skill_tree: unknown requirement '%s' for '%s' in '%s'\n", rsk_name, sk_name, job_name);
							continue;
						}
						ARR_FIND(0, VECTOR_LENGTH(tree_entry->need), l, VECTOR_INDEX(tree_entry->need, l).id == rsk_id);
						if (l == VECTOR_LENGTH(tree_entry->need)) {
							VECTOR_ENSURE(tree_entry->need, 1, 1);
							VECTOR_PUSHZEROED(tree_entry->need);
							req = &VECTOR_LAST(tree_entry->need);
							req->id  = rsk_id;
							req->idx = skill->get_index(rsk_id);
						} else {
							req = &VECTOR_INDEX(tree_entry->need, l);
						}
						req->lv  = (unsigned char)libconfig->setting_get_int(rsk);
					}
				}
			}
		}
	}

	libconfig->destroy(&skill_tree_conf);

	/* lets update all players skill tree */
	iter = mapit_getallusers();
	for (sd = BL_UCAST(BL_PC, mapit->first(iter)); mapit->exists(iter); sd = BL_UCAST(BL_PC, mapit->next(iter)))
		clif->skillinfoblock(sd);
	mapit->free(iter);
}

/**
 * Clears the skill tree and frees any allocated memory.
 */
static void pc_clear_skill_tree(void)
{
	int i;
	for (i = 0; i < CLASS_COUNT; i++) {
		int j;
		for (j = 0; j < MAX_SKILL_TREE; j++) {
			if (pc->skill_tree[i][j].id == 0)
				continue;
			VECTOR_CLEAR(pc->skill_tree[i][j].need);
		}
	}
	memset(pc->skill_tree, 0, sizeof(pc->skill_tree));
}

static bool pc_readdb_levelpenalty(char *fields[], int columns, int current)
{
#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
	int type, race, diff;

	nullpo_retr(false, fields);
	type = atoi(fields[0]);
	race = atoi(fields[1]);
	diff = atoi(fields[2]);

	if( type != 1 && type != 2 ){
		ShowWarning("pc_readdb_levelpenalty: Invalid type %d specified.\n", type);
		return false;
	}

	if (race < RC_FORMLESS || race > RC_MAX) {
		ShowWarning("pc_readdb_levelpenalty: Invalid race %d specified.\n", race);
		return false;
	}

	diff = min(diff, MAX_LEVEL);

	if( diff < 0 )
		diff = min(MAX_LEVEL + ( ~(diff) + 1 ), MAX_LEVEL*2);

	pc->level_penalty[type][race][diff] = atoi(fields[3]);
#endif
	return true;
}

static bool pc_read_exp_db_sub_class(struct config_setting_t *t, bool base)
{
	struct class_exp_group entry = { { 0 } };
	struct config_setting_t *exp_t = NULL;
	int maxlv = 0;

	nullpo_retr(false, t);

	safestrncpy(entry.name, config_setting_name(t), SCRIPT_VARNAME_LENGTH);

	if (libconfig->setting_lookup_int(t, "MaxLevel", &maxlv) == 0
		|| (maxlv <= 0 || maxlv > MAX_LEVEL)) {
		ShowError("pc_read_exp_db_sub_class: Invalid max %s level '%d' set for entry '%s'. Defaulting to %d...", base ? "base" : "job", maxlv, entry.name, MAX_LEVEL);
		maxlv = MAX_LEVEL;
	}

	entry.max_level = maxlv;

	if ((exp_t = libconfig->setting_lookup(t, "Exp")) != NULL && config_setting_is_array(exp_t)) {
		int j = 0;

		VECTOR_ENSURE(entry.exp, maxlv - 2, 10);

		if (libconfig->setting_length(exp_t) > maxlv - 1) {
			ShowWarning("pc_read_exp_db_sub_class: Exp table length (%d) for %s exp group '%s' exceeds specified max level %d. Skipping remaining entries...\n", libconfig->setting_length(exp_t), base ? "base" : "job", entry.name, maxlv);
		}

		while (j < libconfig->setting_length(exp_t) && j <= maxlv - 2)
			VECTOR_PUSH(entry.exp, libconfig->setting_get_int64_elem(exp_t, j++));

		if (j - 1 < maxlv - 2) {
			ShowError("pc_read_exp_db_sub_class: Specified max %d for group '%s', but that group's %s exp table only goes up to level %d.\n", maxlv, entry.name, base ? "base" : "job", VECTOR_LENGTH(entry.exp));
			ShowInfo("Filling the missing values with the last exp entry.\n");
			while (j++ <= maxlv - 2)
				VECTOR_PUSH(entry.exp, VECTOR_LAST(entry.exp));
		}
	} else {
		ShowError("pc_read_exp_db_sub_class: Invalid or non-existent 'Exp' field set for %s level entry '%s'. Skipping...\n", entry.name, base ? "base" : "job");
		return false;
	}

	VECTOR_ENSURE(pc->class_exp_groups[base ? CLASS_EXP_TABLE_BASE : CLASS_EXP_TABLE_JOB], 1, 1);
	VECTOR_PUSH(pc->class_exp_groups[base ? CLASS_EXP_TABLE_BASE : CLASS_EXP_TABLE_JOB], entry);
	return true;
}

/**
 * Description: Helper function to read a root configuration in the exp_group_db.conf file.
 * @param[in]  t       pointer to the root config setting
 * @param[in]  base    boolean switch determining whether to read either base or job exp.
 * @return total number of valid entries read from the setting.
 */
static int pc_read_exp_db_sub(struct config_setting_t *t, bool base)
{
	int i = 0, entry_count = 0;
	struct config_setting_t *tt = NULL;

	nullpo_ret(t);

	while ((tt = libconfig->setting_get_elem(t, i++)) != NULL) {
		pc->read_exp_db_sub_class(tt, base);
		entry_count++;
	}

	return entry_count;
}

/**
 * Description: Initiates reading of the exp_group_db.conf.
 * @return true success, false on failure.
 */
static bool pc_read_exp_db(void)
{
	struct config_t exp_db_conf;
	struct config_setting_t *edb = NULL;
	int entry_count = 0;
	char config_filename[256];

	libconfig->format_db_path(DBPATH"exp_group_db.conf", config_filename, sizeof(config_filename));

	if (!libconfig->load_file(&exp_db_conf, config_filename))
		return false;

	if ((edb = libconfig->setting_lookup(exp_db_conf.root, "base_exp_group_db")) != NULL) {
		entry_count += pc->read_exp_db_sub(edb, true);
	} else {
		ShowError("pc_read_exp_db: Error reading base exp group db in '%s'.\n", config_filename);
		libconfig->destroy(&exp_db_conf);
		return false;
	}

	if ((edb = libconfig->setting_lookup(exp_db_conf.root, "job_exp_group_db")) != NULL) {
		entry_count += pc->read_exp_db_sub(edb, false);
	} else {
		ShowError("pc_read_exp_db: Error reading job exp group db in '%s'.\n", config_filename);
		libconfig->destroy(&exp_db_conf);
		return false;
	}

	libconfig->destroy(&exp_db_conf);

	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", entry_count, config_filename);

	return true;
}

/**
 * Reads the elemental damage modifiers table of a single defending element level.
 * @param def_lv defending element level config
 * @param def_ele defending element id
 * @param lv defending element level
 * @param def_ele_name constant of the defending element (for error messages)
 * @returns number of modifiers read or -1 if something went wrong
 */
static int pc_read_attr_fix_db_level(struct config_setting_t *def_lv, enum elements def_ele, int lv, const char *def_ele_name)
{
	nullpo_retr(-1, def_lv);
	nullpo_retr(-1, def_ele_name);

	struct config_setting_t *atk_attr = NULL;
	int i = 0;
	int count = 0;
	while ((atk_attr = libconfig->setting_get_elem(def_lv, i++)) != NULL) {
		const char *atk_ele_name = config_setting_name(atk_attr);
		int atk_ele;
		if (!script->get_constant(atk_ele_name, &atk_ele)) {
			ShowError("%s: Could not find attacking element '%s'. Skipping entry...\n", __func__, atk_ele_name);
			continue;
		}

		if (atk_ele < ELE_NEUTRAL || atk_ele >= ELE_MAX) {
			ShowError("%s: Invalid element '%s' (%d). Skipping entry...\n", __func__, atk_ele_name, atk_ele);
			continue;
		}

		if (!config_setting_is_number(atk_attr)) {
			ShowError("%s: Damage modifier for element '%s' (%u) attacked by '%s' (%d) is not numeric. Skipping entry...\n", __func__, def_ele_name, def_ele, atk_ele_name, atk_ele);
			continue;
		}

		int dmg_mod = libconfig->setting_get_int(atk_attr);
		battle->attr_fix_table[lv - 1][atk_ele][def_ele] = dmg_mod;
		count++;

#ifndef RENEWAL
		if (battle_config.attr_recover == 0 && battle->attr_fix_table[lv - 1][atk_ele][def_ele] < 0)
			battle->attr_fix_table[lv - 1][atk_ele][def_ele] = 0;
#endif
	}

	return count;
}

/**
 * Reads the elemental damage modifiers table of a single defending element.
 * @param def_attr defending element config
 * @param def_ele defending element id
 * @param def_ele_name constant of the defending element (for error messages)
 * @returns number of modifiers read or -1 if something went wrong
 */
static int pc_read_attr_fix_db_entry(struct config_setting_t *def_attr, enum elements def_ele, const char *def_ele_name)
{
	nullpo_retr(-1, def_attr);
	nullpo_retr(-1, def_ele_name);

	int count = 0;
	for (int i = 1; i <= 4; ++i) {
		char name[5];
		snprintf(name, 5, "Lv%d", i);

		struct config_setting_t *def_lv = libconfig->setting_lookup(def_attr, name);
		if (def_lv != NULL) {
			int result = pc->read_attr_fix_db_level(def_lv, def_ele, i, def_ele_name);
			if (result == -1)
				return -1;

			count += result;
		}
	}

	return count;
}

/**
 * Reads elemental damage modifier table (attr_fix.conf)
 * @returns true if it was loaded (even if partially), false if a fatal error happened.
 */
static bool pc_read_attr_fix_db(void)
{
	// Reset attr_fix to default, with all values as 100%
	for (int i = 0; i < 4; ++i) {
		for (int j = ELE_NEUTRAL; j < ELE_MAX; ++j) {
			for (int k = ELE_NEUTRAL; k < ELE_MAX; ++k)
				battle->attr_fix_table[i][j][k] = 100;
		}
	}

	char filepath[256];
	libconfig->format_db_path(DBPATH"attr_fix.conf", filepath, sizeof(filepath));

	struct config_t attr_fix_conf;
	if (!libconfig->load_file(&attr_fix_conf, filepath))
		return false;

#ifdef ENABLE_CASE_CHECK
	script->parser_current_file = filepath;
#endif // ENABLE_CASE_CHECK

	struct config_setting_t *def_attr = NULL;
	int i = 0;
	int count = 0;
	while ((def_attr = libconfig->setting_get_elem(attr_fix_conf.root, i++)) != NULL) {
		const char *def_ele_name = config_setting_name(def_attr);
		int def_ele;
		if (!script->get_constant(def_ele_name, &def_ele)) {
			ShowError("%s: Could not find defending element '%s'. Skipping entry...\n", __func__, def_ele_name);
			continue;
		}

		if (def_ele < ELE_NEUTRAL || def_ele >= ELE_MAX) {
			ShowError("%s: Invalid element '%s' (%d). Skipping entry...\n", __func__, def_ele_name, def_ele);
			continue;
		}

		int result = pc->read_attr_fix_db_entry(def_attr, (enum elements) def_ele, def_ele_name);
		if (result == -1)
			return false;

		count += result;
	}

#ifdef ENABLE_CASE_CHECK
	script->parser_current_file = NULL;
#endif // ENABLE_CASE_CHECK

	libconfig->destroy(&attr_fix_conf);

	ShowStatus("Done reading '"CL_WHITE"%d"CL_RESET"' entries in '"CL_WHITE"%s"CL_RESET"'.\n", count, filepath);
	return true;
}

/*==========================================
 * PC DB reading.
 * exp_group_db.conf - required experience values
 * skill_tree.txt    - skill tree for every class
 * attr_fix.conf     - elemental adjustment table
 *------------------------------------------*/
static int pc_readdb(void)
{
	/**
	 * Read and load into memory, the exp_group_db.conf file.
	 */
	pc->clear_exp_groups();
	pc->read_exp_db();

	// Reset and read skilltree
	pc->clear_skill_tree();
	pc->read_skill_tree();
#if defined(RENEWAL_DROP) || defined(RENEWAL_EXP)
	sv->readdb(map->db_path, "re/level_penalty.txt", ',', 4, 4, -1, pc->readdb_levelpenalty);
	for (int k = 1; k < 3; k++) { // fill in the blanks
		for (int j = RC_FORMLESS; j < RC_MAX; j++) {
			int tmp = 0;
			for (int i = 0; i < MAX_LEVEL * 2; i++) {
				if( i == MAX_LEVEL+1 )
					tmp = pc->level_penalty[k][j][0];// reset
				if( pc->level_penalty[k][j][i] > 0 )
					tmp = pc->level_penalty[k][j][i];
				else
					pc->level_penalty[k][j][i] = tmp;
			}
		}
	}
#endif

	// Reset then read attr_fix
	if (!pc_read_attr_fix_db())
		return 1;

	// reset then read statspoint
	memset(pc->statp,0,sizeof(pc->statp));
	int i = 1;

	char line[24000];
	sprintf(line, "%s/"DBPATH"statpoint.txt", map->db_path);
	FILE *fp = fopen(line, "r");
	if(fp == NULL){
		ShowWarning("Can't read '"CL_WHITE"%s"CL_RESET"'... Generating DB.\n",line);
		//return 1;
	} else {
		unsigned int count = 0;

		while(fgets(line, sizeof(line), fp))
		{
			int stat;
			if(line[0]=='/' && line[1]=='/')
				continue;
			if ((stat=(int)strtol(line,NULL,10))<0)
				stat=0;
			if (i > MAX_LEVEL)
				break;
			count++;
			pc->statp[i]=stat;
			i++;
		}
		fclose(fp);

		ShowStatus("Done reading '"CL_WHITE"%u"CL_RESET"' entries in '"CL_WHITE"%s/"DBPATH"%s"CL_RESET"'.\n",count,map->db_path,"statpoint.txt");
	}
	// generate the remaining parts of the db if necessary
	int k = battle_config.use_statpoint_table; //save setting
	battle_config.use_statpoint_table = 0; //temporarily disable to force pc->gets_status_point use default values
	pc->statp[0] = 45; // seed value
	for (; i <= MAX_LEVEL; i++)
		pc->statp[i] = pc->statp[i-1] + pc->gets_status_point(i-1);
	battle_config.use_statpoint_table = k; //restore setting

	return 0;
}

static bool pc_job_is_dummy(int job)
{
	if (job == JOB_KNIGHT2      || job == JOB_CRUSADER2
	 || job == JOB_WEDDING      || job == JOB_XMAS || job == JOB_SUMMER
	 || job == JOB_LORD_KNIGHT2 || job == JOB_PALADIN2
	 || job == JOB_BABY_KNIGHT2 || job == JOB_BABY_CRUSADER2
	 || job == JOB_STAR_GLADIATOR2 || job == JOB_BABY_STAR_GLADIATOR2
	 || (job >= JOB_RUNE_KNIGHT2 && job <= JOB_MECHANIC_T2)
	 || (job >= JOB_BABY_RUNE2 && job <= JOB_BABY_MECHANIC2)
	 || job == JOB_DUMMY4219    || job == JOB_DUMMY4221
	 || (job >= JOB_DUMMY4230 && job <= JOB_DUMMY4237))
		return true;
	return false;
}

static void pc_validate_levels(void)
{
	int i;
	int j;
	for (i = 0; i < JOB_MAX; i++) {
		if (!pc->db_checkid(i)) continue;
		if (pc->job_is_dummy(i))
			continue; //Classes that do not need exp tables.
		j = pc->class2idx(i);
		if (pc->dbs->class_exp_table[j][CLASS_EXP_TABLE_BASE] == NULL)
			ShowWarning("Class %s (%d - %d) does not have a base exp table.\n", pc->job_name(i), i, j);
		if (pc->dbs->class_exp_table[j][CLASS_EXP_TABLE_JOB] == NULL)
			ShowWarning("Class %s (%d - %d) does not have a job exp table.\n", pc->job_name(i), i, j);
	}
}

static void pc_itemcd_do(struct map_session_data *sd, bool load)
{
	int i,cursor = 0;
	struct item_cd* cd = NULL;

	nullpo_retv(sd);
	if( load ) {
		if( !(cd = idb_get(pc->itemcd_db, sd->status.char_id)) ) {
			// no skill cooldown is associated with this character
			return;
		}
		for(i = 0; i < MAX_ITEMDELAYS; i++) {
			if( cd->nameid[i] && DIFF_TICK(timer->gettick(),cd->tick[i]) < 0 ) {
				sd->item_delay[cursor].tick = cd->tick[i];
				sd->item_delay[cursor].nameid = cd->nameid[i];
				cursor++;
			}
		}
		idb_remove(pc->itemcd_db,sd->status.char_id);
	} else {
		if( !(cd = idb_get(pc->itemcd_db,sd->status.char_id)) ) {
			// create a new skill cooldown object for map storage
			CREATE( cd, struct item_cd, 1 );
			idb_put( pc->itemcd_db, sd->status.char_id, cd );
		}
		for(i = 0; i < MAX_ITEMDELAYS; i++) {
			if( sd->item_delay[i].nameid && DIFF_TICK(timer->gettick(),sd->item_delay[i].tick) < 0 ) {
				cd->tick[cursor] = sd->item_delay[i].tick;
				cd->nameid[cursor] = sd->item_delay[i].nameid;
				cursor++;
			}
		}
	}
	return;
}

static void pc_bank_deposit(struct map_session_data *sd, int money)
{
	unsigned int limit_check;

	nullpo_retv(sd);
	limit_check = money + sd->status.bank_vault;

	if( money <= 0 || limit_check > MAX_BANK_ZENY ) {
		clif->bank_deposit(sd,BDA_OVERFLOW);
		return;
	} else if ( money > sd->status.zeny ) {
		clif->bank_deposit(sd,BDA_NO_MONEY);
		return;
	}

	if( pc->payzeny(sd,money, LOG_TYPE_BANK, NULL) )
		clif->bank_deposit(sd,BDA_NO_MONEY);
	else {
		sd->status.bank_vault += money;
		if( map->save_settings&256 )
			chrif->save(sd,0);
		clif->bank_deposit(sd,BDA_SUCCESS);
	}
}
static void pc_bank_withdraw(struct map_session_data *sd, int money)
{
	unsigned int limit_check;

	nullpo_retv(sd);
	limit_check = money + sd->status.zeny;
	if (money <= 0) {
		clif->bank_withdraw(sd,BWA_UNKNOWN_ERROR);
		return;
	} else if (money > sd->status.bank_vault) {
		clif->bank_withdraw(sd,BWA_NO_MONEY);
		return;
	} else if (limit_check > MAX_ZENY) {
		/* no official response for this scenario exists. */
		clif->messagecolor_self(sd->fd, COLOR_RED, msg_sd(sd,1482));
		return;
	}

	if( pc->getzeny(sd,money, LOG_TYPE_BANK, NULL) )
		clif->bank_withdraw(sd,BWA_NO_MONEY);
	else {
		sd->status.bank_vault -= money;
		if( map->save_settings&256 )
			chrif->save(sd,0);
		clif->bank_withdraw(sd,BWA_SUCCESS);
	}
}
/* status change data arrived from char-server */
static void pc_scdata_received(struct map_session_data *sd)
{
	nullpo_retv(sd);
	pc->inventory_rentals(sd);

	if (sd->expiration_time != 0) { // don't display if it's unlimited or unknow value
		time_t exp_time = sd->expiration_time;
		char tmpstr[1024];
		strftime(tmpstr, sizeof(tmpstr) - 1, msg_sd(sd,501), localtime(&exp_time)); // "Your account time limit is: %d-%m-%Y %H:%M:%S."
		clif->wis_message(sd->fd, map->wisp_server_name, tmpstr, (int)strlen(tmpstr));

		pc->expire_check(sd);
	}

	if( sd->state.standalone ) {
		clif->pLoadEndAck(0,sd);
		pc->autotrade_populate(sd);
		pc->autotrade_start(sd);
	}

	if (sd->sc.data[SC_SOULENERGY] != NULL)
		sd->soulball = sd->sc.data[SC_SOULENERGY]->val1;
}
static int pc_expiration_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct map_session_data *sd = map->id2sd(id);

	if( !sd ) return 0;

	sd->expiration_tid = INVALID_TIMER;

	if( sd->fd )
		clif->authfail_fd(sd->fd,10);

	map->quit(sd);

	return 0;
}
/* This timer exists only when a character with an expire timer > 24h is online */
/* It loops through online players once an hour to check whether a new < 24h is available */
static int pc_global_expiration_timer(int tid, int64 tick, int id, intptr_t data)
{
	struct s_mapiterator* iter;
	struct map_session_data* sd;

	iter = mapit_getallusers();
	for (sd = BL_UCAST(BL_PC, mapit->first(iter)); mapit->exists(iter); sd = BL_UCAST(BL_PC, mapit->next(iter))) {
		if( sd->expiration_time )
			pc->expire_check(sd);
	}
	mapit->free(iter);

	return 0;
}
static void pc_expire_check(struct map_session_data *sd)
{
	nullpo_retv(sd);
	/* ongoing timer */
	if( sd->expiration_tid != INVALID_TIMER )
		return;

	/* not within the next 24h, enable the global check */
	if( sd->expiration_time > ( time(NULL) + ( ( 60 * 60 ) * 24 ) ) ) {
		/* global check not running, enable */
		if( pc->expiration_tid == INVALID_TIMER ) {
			/* starts in 1h, repeats every hour */
			pc->expiration_tid = timer->add_interval(timer->gettick() + ((1000*60)*60), pc->global_expiration_timer, 0, 0, ((1000*60)*60));
		}
		return;
	}

	sd->expiration_tid = timer->add(timer->gettick() + (int64)(sd->expiration_time - time(NULL))*1000, pc->expiration_timer, sd->bl.id, 0);
}
/**
 * Loads autotraders
 ***/
static void pc_autotrade_load(void)
{
	char *data;

	if (SQL_ERROR == SQL->Query(map->mysql_handle, "SELECT `account_id`,`char_id`,`sex`,`title` FROM `%s`",map->autotrade_merchants_db))
		Sql_ShowDebug(map->mysql_handle);

	while (SQL_SUCCESS == SQL->NextRow(map->mysql_handle)) {
		struct map_session_data *sd;
		int account_id, char_id;
		char title[MESSAGE_SIZE];
		unsigned char sex;

		SQL->GetData(map->mysql_handle, 0, &data, NULL); account_id = atoi(data);
		SQL->GetData(map->mysql_handle, 1, &data, NULL); char_id = atoi(data);
		SQL->GetData(map->mysql_handle, 2, &data, NULL); sex = atoi(data);
		SQL->GetData(map->mysql_handle, 3, &data, NULL); safestrncpy(title, data, sizeof(title));

		CREATE(sd, struct map_session_data, 1);

		pc->setnewpc(sd, account_id, char_id, 0, 0, sex, 0);

		safestrncpy(sd->message, title, MESSAGE_SIZE);
		sd->state.standalone = 1;
		sd->group = pcg->get_dummy_group();

		chrif->authreq(sd,true);
	}
	SQL->FreeResult(map->mysql_handle);
}
/**
 * Loads vending data and sets it up, is triggered when char server data that pc_autotrade_load requested arrives
 **/
static void pc_autotrade_start(struct map_session_data *sd)
{
	unsigned int count = 0;
	int i;
	char *data;

	nullpo_retv(sd);
	if (SQL_ERROR == SQL->Query(map->mysql_handle, "SELECT `itemkey`,`amount`,`price` FROM `%s` WHERE `char_id` = '%d'",map->autotrade_data_db,sd->status.char_id))
		Sql_ShowDebug(map->mysql_handle);

	while( SQL_SUCCESS == SQL->NextRow(map->mysql_handle) ) {
		int itemkey, amount, price;

		SQL->GetData(map->mysql_handle, 0, &data, NULL); itemkey = atoi(data);
		SQL->GetData(map->mysql_handle, 1, &data, NULL); amount = atoi(data);
		SQL->GetData(map->mysql_handle, 2, &data, NULL); price = atoi(data);

		ARR_FIND(0, MAX_CART, i, sd->status.cart[i].id == itemkey);
		if( i != MAX_CART && itemdb_cantrade(&sd->status.cart[i], 0, 0) ) {
			if( amount > sd->status.cart[i].amount )
				amount = sd->status.cart[i].amount;

			if( amount ) {
				sd->vending[count].index = i;
				sd->vending[count].amount = amount;
				sd->vending[count].value = cap_value(price, 0, (unsigned int)battle_config.vending_max_value);

				count++;
			}
		}
	}

	if( !count ) {
		pc->autotrade_update(sd,PAUC_REMOVE);
		map->quit(sd);
	} else {
		sd->state.autotrade = 1;
		sd->vender_id = ++vending->next_id;
		sd->vend_num = count;
		sd->state.vending = true;
		idb_put(vending->db, sd->status.char_id, sd);
		if( map->list[sd->bl.m].users )
			clif->showvendingboard(&sd->bl,sd->message,0);
	}
}
/**
 * Perform a autotrade action
 **/
static void pc_autotrade_update(struct map_session_data *sd, enum e_pc_autotrade_update_action action)
{
	int i;

	nullpo_retv(sd);
	/* either way, this goes down */
	if( action != PAUC_START ) {
		if (SQL_ERROR == SQL->Query(map->mysql_handle, "DELETE FROM `%s` WHERE `char_id` = '%d'",map->autotrade_data_db,sd->status.char_id))
			Sql_ShowDebug(map->mysql_handle);
	}

	switch( action ) {
		case PAUC_REMOVE:
			if (SQL_ERROR == SQL->Query(map->mysql_handle, "DELETE FROM `%s` WHERE `char_id` = '%d' LIMIT 1",map->autotrade_merchants_db,sd->status.char_id))
				Sql_ShowDebug(map->mysql_handle);
			break;
		case PAUC_START: {
			char title[MESSAGE_SIZE*2+1];

			SQL->EscapeStringLen(map->mysql_handle, title, sd->message, strnlen(sd->message, MESSAGE_SIZE));

			if (SQL_ERROR == SQL->Query(map->mysql_handle, "INSERT INTO `%s` (`account_id`,`char_id`,`sex`,`title`) VALUES ('%d','%d','%d','%s')",
										map->autotrade_merchants_db,
										sd->status.account_id,
										sd->status.char_id,
										sd->status.sex,
										title
										))
				Sql_ShowDebug(map->mysql_handle);
		}
		FALLTHROUGH
		case PAUC_REFRESH:
			for( i = 0; i < sd->vend_num; i++ ) {
				if( sd->vending[i].amount == 0 )
					continue;

				if (SQL_ERROR == SQL->Query(map->mysql_handle, "INSERT INTO `%s` (`char_id`,`itemkey`,`amount`,`price`) VALUES ('%d','%d','%d','%u')",
											map->autotrade_data_db,
											sd->status.char_id,
											sd->status.cart[sd->vending[i].index].id,
											sd->vending[i].amount,
											sd->vending[i].value
											))
					Sql_ShowDebug(map->mysql_handle);
			}
			break;
	}
}
/**
 * Handles characters upon @autotrade usage
 **/
static void pc_autotrade_prepare(struct map_session_data *sd)
{
	struct autotrade_vending *data;
	int i, cursor = 0;
	int account_id, char_id;
	char title[MESSAGE_SIZE];
	unsigned char sex;

	nullpo_retv(sd);
	CREATE(data, struct autotrade_vending, 1);

	memcpy(data->vending, sd->vending, sizeof(sd->vending));

	for(i = 0; i < sd->vend_num; i++) {
		if( sd->vending[i].amount ) {
			memcpy(&data->list[cursor],&sd->status.cart[sd->vending[i].index],sizeof(struct item));
			cursor++;
		}
	}

	data->vend_num = (unsigned char)cursor;

	idb_put(pc->at_db, sd->status.char_id, data);

	account_id = sd->status.account_id;
	char_id = sd->status.char_id;
	sex = sd->status.sex;
	safestrncpy(title, sd->message, sizeof(title));

	sd->npc_id = 0;
	sd->npc_shopid = 0;
	if (sd->st) {
		sd->st->state = END;
		sd->st = NULL;
	}
	map->quit(sd);
	chrif->auth_delete(account_id, char_id, ST_LOGOUT);

	CREATE(sd, struct map_session_data, 1);

	pc->setnewpc(sd, account_id, char_id, 0, 0, sex, 0);

	safestrncpy(sd->message, title, MESSAGE_SIZE);
	sd->state.standalone = 1;
	sd->group = pcg->get_dummy_group();

	chrif->authreq(sd,true);
}
/**
 * Prepares autotrade data from pc->at_db from a player that has already returned from char server
 **/
static void pc_autotrade_populate(struct map_session_data *sd)
{
	struct autotrade_vending *data;
	int i, j, k, cursor = 0;

	nullpo_retv(sd);
	if( !(data = idb_get(pc->at_db,sd->status.char_id)) )
		return;

	for(i = 0; i < data->vend_num; i++) {
		if( !data->vending[i].amount )
			continue;

		for(j = 0; j < MAX_CART; j++) {
			if( !memcmp((char*)(&data->list[i]) + sizeof(data->list[0].id), (char*)(&sd->status.cart[j]) + sizeof(data->list[0].id), sizeof(struct item) - sizeof(data->list[0].id)) ) {
				if( cursor ) {
					ARR_FIND(0, cursor, k, sd->vending[k].index == j);
					if( k != cursor )
						continue;
				}
				break;
			}
		}

		if( j != MAX_CART ) {
			sd->vending[cursor].index = j;
			sd->vending[cursor].amount = data->vending[i].amount;
			sd->vending[cursor].value = data->vending[i].value;

			cursor++;
		}
	}

	sd->vend_num = cursor;

	pc->autotrade_update(sd,PAUC_START);

	HPM->data_store_destroy(&data->hdata);

	idb_remove(pc->at_db, sd->status.char_id);
}

/**
 * @see DBApply
 */
static int pc_autotrade_final(union DBKey key, struct DBData *data, va_list ap)
{
	struct autotrade_vending* at_v = DB->data2ptr(data);
	nullpo_ret(at_v);
	HPM->data_store_destroy(&at_v->hdata);
	return 0;
}

static void pc_update_idle_time(struct map_session_data *sd, enum e_battle_config_idletime type)
{
	nullpo_retv(sd);
	if (battle_config.idletime_criteria&type)
		sd->idletime = sockt->last_tick;
}

//Checks if the given class value corresponds to a player class. [Skotlex]
//JOB_NOVICE isn't checked for class is supposed to be unsigned
static bool pc_db_checkid(int class)
{
	return class < JOB_MAX_BASIC
		|| (class >= JOB_NOVICE_HIGH    && class <= JOB_DARK_COLLECTOR )
		|| (class >= JOB_RUNE_KNIGHT    && class <= JOB_MECHANIC_T2    )
		|| (class >= JOB_BABY_RUNE      && class <= JOB_BABY_MECHANIC2 )
		|| (class >= JOB_SUPER_NOVICE_E && class <= JOB_SUPER_BABY_E   )
		|| (class >= JOB_KAGEROU        && class <= JOB_OBORO          )
		|| (class == JOB_REBELLION)
		|| (class >= JOB_SUMMONER       && class <  JOB_MAX            );
}

/**
 * checks if player have any kind of magnifier in inventory
 * @param sd map_session_data of Player
 * @return index of magnifer, INDEX_NOT_FOUND if it is not found
 */
static int pc_have_magnifier(struct map_session_data *sd)
{
	int n;
	n = pc->search_inventory(sd, ITEMID_SPECTACLES);
	if (n == INDEX_NOT_FOUND)
		n = pc->search_inventory(sd, ITEMID_N_MAGNIFIER);
	return n;
}

/**
 * checks if player have any item that listed in item chain
 * @param sd map_session_data of Player
 * @param chain_cache_id cache id of item chain
 * @return index of inventory, INDEX_NOT_FOUND if it is not found
 */
static int pc_have_item_chain(struct map_session_data *sd, enum e_chain_cache chain_cache_id)
{
	nullpo_retr(INDEX_NOT_FOUND, sd);
	Assert_retr(INDEX_NOT_FOUND, chain_cache_id >= ECC_ORE && chain_cache_id < ECC_MAX);

	int chain_id = itemdb->chain_cache[chain_cache_id];

	for (int n = 0; n < itemdb->chains[chain_id].qty; n++) {
		struct item_chain_entry *entry = &itemdb->chains[chain_id].items[n];
		int index = pc->search_inventory(sd, entry->id);
		if (index != INDEX_NOT_FOUND)
			return index;
	}

	return INDEX_NOT_FOUND;
}

/**
 * Checks if player have basic skills learned.
 * @param  sd Player Data
 * @param  level Required Level of Novice Skill
 * @return bool true, if requirement is satisfied
 */
static bool pc_check_basicskill(struct map_session_data *sd, int level)
{
	if (pc->checkskill(sd, NV_BASIC) >= level || pc->checkskill(sd, SU_BASIC_SKILL))
		return true;
	return false;
}

/**
 * Verifies a chat message, searching for atcommands, checking if the sender
 * character can chat, and updating the idle timer.
 *
 * @param sd      The sender character.
 * @param message The message text.
 * @return Whether the message is a valid chat message.
 */
static bool pc_process_chat_message(struct map_session_data *sd, const char *message)
{
	nullpo_retr(false, sd);
	if (atcommand->exec(sd->fd, sd, message, true)) {
		return false;
	}

	if (!pc->can_talk(sd)) {
		return false;
	}

	if (battle_config.min_chat_delay != 0) {
		if (DIFF_TICK(sd->cantalk_tick, timer->gettick()) > 0) {
			return false;
		}
		sd->cantalk_tick = timer->gettick() + battle_config.min_chat_delay;
	}

	pc->update_idle_time(sd, BCIDLE_CHAT);

	return true;
}

/**
 * Checks a chat message, scanning for the Super Novice prayer sequence.
 *
 * If a match is found, the angel is invoked or the counter is incremented as
 * appropriate.
 *
 * @param sd      The sender character.
 * @param message The message text.
 */
static void pc_check_supernovice_call(struct map_session_data *sd, const char *message)
{
	uint64 next = pc->nextbaseexp(sd);
	int percent = 0;

	nullpo_retv(sd);
	nullpo_retv(message);
	if ((sd->job & MAPID_UPPERMASK) != MAPID_SUPER_NOVICE)
		return;
	if (next == 0)
		next = pc->thisbaseexp(sd);
	if (next == 0)
		return;

	// 0%, 10%, 20%, ...
	percent = (int)( ( (float)sd->status.base_exp/(float)next )*1000. );
	if ((battle_config.snovice_call_type != 0 || percent != 0) && (percent%100) == 0) {
		// 10.0%, 20.0%, ..., 90.0%
		switch (sd->state.snovice_call_flag) {
		case 0:
			if (strstr(message, msg_txt(1479))) // "Dear angel, can you hear my voice?"
				sd->state.snovice_call_flag = 1;
			break;
		case 1:
		{
			char buf[256];
			snprintf(buf, 256, msg_txt(1480), sd->status.name);
			if (strstr(message, buf)) // "I am %s Super Novice~"
				sd->state.snovice_call_flag = 2;
		}
			break;
		case 2:
			if (strstr(message, msg_txt(1481))) // "Help me out~ Please~ T_T"
				sd->state.snovice_call_flag = 3;
			break;
		case 3:
			sc_start(NULL, &sd->bl, skill->get_sc_type(MO_EXPLOSIONSPIRITS), 100, 17, skill->get_time(MO_EXPLOSIONSPIRITS, 5), MO_EXPLOSIONSPIRITS); // Lv17-> +50 critical (noted by Poki) [Skotlex]
			clif->skill_nodamage(&sd->bl, &sd->bl, MO_EXPLOSIONSPIRITS, 5, 1);  // prayer always shows successful Lv5 cast and disregards noskill restrictions
			sd->state.snovice_call_flag = 0;
			break;
		}
	}
}

/**
 * Sends a message t all online GMs having the specified permission.
 *
 * @param sender_name  Sender character name.
 * @param permission   The required permission to receive this message.
 * @param message      The message body.
 *
 * @return The amount of characters the message was delivered to.
 */
// The transmission of GM only Wisp/Page from server to inter-server
static int pc_wis_message_to_gm(const char *sender_name, int permission, const char *message)
{
	nullpo_ret(sender_name);
	nullpo_ret(message);
	int mes_len = (int)strlen(message) + 1; // + null
	int count = 0;

	// information is sent to all online GM
	map->foreachpc(pc->wis_message_to_gm_sub, permission, sender_name, message, mes_len, &count);

	return count;
}

/**
 * Helper function for pc_wis_message_to_gm().
 */
static int pc_wis_message_to_gm_sub(struct map_session_data *sd, va_list va)
{
	nullpo_ret(sd);

	int permission = va_arg(va, int);
	if (!pc_has_permission(sd, permission))
		return 0;

	const char *sender_name = va_arg(va, const char *);
	const char *message = va_arg(va, const char *);
	int len = va_arg(va, int);
	int *count = va_arg(va, int *);

	nullpo_ret(sender_name);
	nullpo_ret(message);
	nullpo_ret(count);

	clif->wis_message(sd->fd, sender_name, message, len);
	++*count;
	return 1;
}

static void pc_update_job_and_level(struct map_session_data *sd)
{
	nullpo_retv(sd);

	if (sd->status.party_id) {
		struct party_data *p;
		int i;

		if ((p = party->search(sd->status.party_id)) != NULL) {
			ARR_FIND(0, MAX_PARTY, i, p->party.member[i].char_id == sd->status.char_id);
			if (i < MAX_PARTY) {
				p->party.member[i].lv = sd->status.base_level;
				clif->party_job_and_level(sd);
			}
		}
	}
}

static void pc_clear_exp_groups(void)
{
	int i, k, size;
	for (k = 0; k < 2; k++) {
		size = VECTOR_LENGTH(pc->class_exp_groups[k]);

		for (i = 0; i < size; i++)
			VECTOR_CLEAR(VECTOR_INDEX(pc->class_exp_groups[k], i).exp);
		VECTOR_CLEAR(pc->class_exp_groups[k]);
	}
}

static void pc_init_exp_groups(void)
{
	int i;
	for (i = 0; i < 2; i++) {
		VECTOR_INIT(pc->class_exp_groups[i]);
	}
}

static bool pc_has_second_costume(struct map_session_data *sd)
{
	nullpo_retr(false, sd);

//	FIXME: JOB_SUPER_NOVICE_E(4190) is not supposed to be 3rd Job. (Issue#2383)
	if ((sd->job & JOBL_THIRD) != 0 && (sd->job & MAPID_BASEMASK) != MAPID_NOVICE)
		return true;
	return false;
}

static bool pc_expandInventory(struct map_session_data *sd, int adjustSize)
{
	nullpo_retr(false, sd);
	const int invSize = sd->status.inventorySize;
	if (adjustSize > MAX_INVENTORY || invSize + adjustSize <= FIXED_INVENTORY_SIZE || invSize + adjustSize > MAX_INVENTORY) {
		clif->inventoryExpandResult(sd, EXPAND_INVENTORY_RESULT_MAX_SIZE);
		return false;
	}
	if (pc_isdead(sd) || sd->state.vending || sd->state.prevend || sd->state.buyingstore || sd->chat_id != 0 || sd->state.trading || sd->state.storage_flag || sd->state.prevend) {
		clif->inventoryExpandResult(sd, EXPAND_INVENTORY_RESULT_OTHER_WORK);
		return false;
	}
	sd->status.inventorySize += adjustSize;
	clif->inventoryExpansionInfo(sd);
	return true;
}

static bool pc_auto_exp_insurance(struct map_session_data *sd)
{
	nullpo_retr(false, sd);

	int item_position = pc->have_item_chain(sd, ECC_NEO_INSURANCE);
	if (item_position == INDEX_NOT_FOUND)
		return false;

	pc->delitem(sd, item_position, 1, 0, DELITEM_SKILLUSE, LOG_TYPE_CONSUME);
#if PACKETVER >= 20100914
	clif->msgtable(sd, MSG_NOTIFY_NEO_INSURANCE_ITEM_USE);
#endif
	return true;
}

/**
* Clear Crimson Marker data from caster
* @param sd: Player
**/
void pc_crimson_marker_clear(struct map_session_data *sd)
{
	nullpo_retv(sd);

	for (int i = 0; i < MAX_SKILL_CRIMSON_MARKER; i++) {
		struct block_list *bl = NULL;
		if (sd->c_marker[i] && (bl = map->id2bl(sd->c_marker[i])))
			status_change_end(bl, SC_CRIMSON_MARKER, INVALID_TIMER);
		sd->c_marker[i] = 0;
	}
}

static void do_final_pc(void)
{

	db_destroy(pc->itemcd_db);
	pc->at_db->destroy(pc->at_db,pc->autotrade_final);

	pcg->final();

	pc->clear_skill_tree();

	pc->clear_exp_groups();

	ers_destroy(pc->sc_display_ers);
	ers_destroy(pc->num_reg_ers);
	ers_destroy(pc->str_reg_ers);

	return;
}

static void do_init_pc(bool minimal)
{
	if (minimal)
		return;

	pc->itemcd_db = idb_alloc(DB_OPT_RELEASE_DATA);
	pc->at_db = idb_alloc(DB_OPT_RELEASE_DATA);

	pc->init_exp_groups();
	pc->readdb();

	timer->add_func_list(pc->invincible_timer, "pc_invincible_timer");
	timer->add_func_list(pc->eventtimer, "pc_eventtimer");
	timer->add_func_list(pc->inventory_rental_end, "pc_inventory_rental_end");
	timer->add_func_list(pc->calc_pvprank_timer, "pc_calc_pvprank_timer");
	timer->add_func_list(pc->autosave, "pc_autosave");
	timer->add_func_list(pc->spiritball_timer, "pc_spiritball_timer");
	timer->add_func_list(pc->follow_timer, "pc_follow_timer");
	timer->add_func_list(pc->endautobonus, "pc_endautobonus");
	timer->add_func_list(pc->charm_timer, "pc_charm_timer");
	timer->add_func_list(pc->global_expiration_timer,"pc_global_expiration_timer");
	timer->add_func_list(pc->expiration_timer,"pc_expiration_timer");

	timer->add(timer->gettick() + map->autosave_interval, pc->autosave, 0, 0);

	// 0=day, 1=night [Yor]
	map->night_flag = battle_config.night_at_start ? 1 : 0;

	if (battle_config.day_duration > 0 && battle_config.night_duration > 0) {
		int day_duration = battle_config.day_duration;
		int night_duration = battle_config.night_duration;
		// add night/day timer [Yor]
		timer->add_func_list(pc->map_day_timer, "pc_map_day_timer");
		timer->add_func_list(pc->map_night_timer, "pc_map_night_timer");

		pc->day_timer_tid   = timer->add_interval(timer->gettick() + (map->night_flag ? 0 : day_duration) + night_duration, pc->map_day_timer,   0, 0, day_duration + night_duration);
		pc->night_timer_tid = timer->add_interval(timer->gettick() + day_duration + (map->night_flag ? night_duration : 0), pc->map_night_timer, 0, 0, day_duration + night_duration);
	}

	pcg->init();

	pc->sc_display_ers = ers_new(sizeof(struct sc_display_entry), "pc.c:sc_display_ers", ERS_OPT_FLEX_CHUNK);
	pc->num_reg_ers = ers_new(sizeof(struct script_reg_num), "pc.c::num_reg_ers", ERS_OPT_CLEAN|ERS_OPT_FLEX_CHUNK);
	pc->str_reg_ers = ers_new(sizeof(struct script_reg_str), "pc.c::str_reg_ers", ERS_OPT_CLEAN|ERS_OPT_FLEX_CHUNK);

	ers_chunk_size(pc->sc_display_ers, 150);
	ers_chunk_size(pc->num_reg_ers, 300);
	ers_chunk_size(pc->str_reg_ers, 50);
}

/*=====================================
 * Default Functions : pc.h
 * Generated by HerculesInterfaceMaker
 * created by Susu
 *-------------------------------------*/
void pc_defaults(void)
{
	const struct sg_data sg_info[MAX_PC_FEELHATE] = {
		{ SG_SUN_ANGER, SG_SUN_BLESS, SG_SUN_COMFORT, "PC_FEEL_SUN", "PC_HATE_MOB_SUN", is_day_of_sun },
		{ SG_MOON_ANGER, SG_MOON_BLESS, SG_MOON_COMFORT, "PC_FEEL_MOON", "PC_HATE_MOB_MOON", is_day_of_moon },
		{ SG_STAR_ANGER, SG_STAR_BLESS, SG_STAR_COMFORT, "PC_FEEL_STAR", "PC_HATE_MOB_STAR", is_day_of_star }
	};
	unsigned int equip_pos[EQI_MAX]={EQP_ACC_L,EQP_ACC_R,EQP_SHOES,EQP_GARMENT,EQP_HEAD_LOW,EQP_HEAD_MID,EQP_HEAD_TOP,EQP_ARMOR,EQP_HAND_L,EQP_HAND_R,EQP_COSTUME_HEAD_TOP,EQP_COSTUME_HEAD_MID,EQP_COSTUME_HEAD_LOW,EQP_COSTUME_GARMENT,EQP_AMMO, EQP_SHADOW_ARMOR, EQP_SHADOW_WEAPON, EQP_SHADOW_SHIELD, EQP_SHADOW_SHOES, EQP_SHADOW_ACC_R, EQP_SHADOW_ACC_L };

	pc = &pc_s;
	pc->dbs = &exptables;

	/* vars */
	pc->at_db = NULL;
	pc->itemcd_db = NULL;
	/* */
	pc->day_timer_tid = INVALID_TIMER;
	pc->night_timer_tid = INVALID_TIMER;

	// These macros are used instead of a sum of sizeof(), to ensure that padding won't interfere with our size, and code won't rot when adding more fields
	memset(ZEROED_BLOCK_POS(pc), 0, ZEROED_BLOCK_SIZE(pc));

	/* */
	memcpy(pc->equip_pos, &equip_pos, sizeof(pc->equip_pos));
	/* */
	memcpy(pc->sg_info, sg_info, sizeof(pc->sg_info));
	/* */
	pc->sc_display_ers = NULL;
	/* */
	pc->expiration_tid = INVALID_TIMER;
	/* */
	pc->num_reg_ers = NULL;
	pc->str_reg_ers = NULL;
	/* */
	pc->reg_load = false;
	/* funcs */
	pc->init = do_init_pc;
	pc->final = do_final_pc;

	pc->get_dummy_sd = pc_get_dummy_sd;
	pc->class2idx = pc_class2idx;

	pc->can_use_command = pc_can_use_command;
	pc->set_group = pc_set_group;
	pc->should_log_commands = pc_should_log_commands;

	pc->setrestartvalue = pc_setrestartvalue;
	pc->makesavestatus = pc_makesavestatus;
	pc->respawn = pc_respawn;
	pc->setnewpc = pc_setnewpc;
	pc->authok = pc_authok;
	pc->authfail = pc_authfail;
	pc->reg_received = pc_reg_received;

	pc->isequip = pc_isequip;
	pc->equippoint = pc_equippoint;
	pc->item_equippoint = pc_item_equippoint;
	pc->setinventorydata = pc_setinventorydata;

	pc->checkskill = pc_checkskill;
	pc->checkskill2 = pc_checkskill2;
	pc->checkallowskill = pc_checkallowskill;
	pc->checkequip = pc_checkequip;
	pc->get_skill_cooldown = pc_get_skill_cooldown;

	pc->calc_skilltree = pc_calc_skilltree;
	pc->calc_skilltree_bonus = pc_calc_skilltree_bonus;
	pc->calc_skilltree_clear = pc_calc_skilltree_clear;
	pc->calc_skilltree_normalize_job = pc_calc_skilltree_normalize_job;
	pc->clean_skilltree = pc_clean_skilltree;

	pc->setpos = pc_setpos;
	pc->setsavepoint = pc_setsavepoint;
	pc->randomwarp = pc_randomwarp;
	pc->memo = pc_memo;

	pc->checkadditem = pc_checkadditem;
	pc->inventoryblank = pc_inventoryblank;
	pc->search_inventory = pc_search_inventory;
	pc->payzeny = pc_payzeny;
	pc->additem = pc_additem;
	pc->getzeny = pc_getzeny;
	pc->delitem = pc_delitem;
	// Special Shop System
	pc->paycash = pc_paycash;
	pc->getcash = pc_getcash;

	pc->cart_additem = pc_cart_additem;
	pc->cart_delitem = pc_cart_delitem;
	pc->putitemtocart = pc_putitemtocart;
	pc->getitemfromcart = pc_getitemfromcart;
	pc->cartitem_amount = pc_cartitem_amount;

	pc->takeitem = pc_takeitem;
	pc->dropitem = pc_dropitem;

	pc->isequipped = pc_isequipped;
	pc->can_Adopt = pc_can_Adopt;
	pc->adoption = pc_adoption;

	pc->updateweightstatus = pc_updateweightstatus;

	pc->addautobonus = pc_addautobonus;
	pc->exeautobonus = pc_exeautobonus;
	pc->endautobonus = pc_endautobonus;
	pc->delautobonus = pc_delautobonus;

	pc->bonus_addele = pc_bonus_addele;
	pc->bonus_subele = pc_bonus_subele;

	pc->bonus = pc_bonus;
	pc->bonus2 = pc_bonus2;
	pc->bonus3 = pc_bonus3;
	pc->bonus4 = pc_bonus4;
	pc->bonus5 = pc_bonus5;
	pc->skill = pc_skill;

	pc->insert_card = pc_insert_card;
	pc->can_insert_card = pc_can_insert_card;
	pc->can_insert_card_into = pc_can_insert_card_into;

	pc->steal_item = pc_steal_item;
	pc->steal_coin = pc_steal_coin;

	pc->modifybuyvalue = pc_modifybuyvalue;
	pc->modifysellvalue = pc_modifysellvalue;

	pc->follow = pc_follow; // [MouseJstr]
	pc->stop_following = pc_stop_following;

	pc->maxbaselv = pc_maxbaselv;
	pc->maxjoblv = pc_maxjoblv;
	pc->checkbaselevelup = pc_checkbaselevelup;
	pc->checkbaselevelup_sc = pc_checkbaselevelup_sc;
	pc->checkjoblevelup = pc_checkjoblevelup;
	pc->gainexp = pc_gainexp;
	pc->nextbaseexp = pc_nextbaseexp;
	pc->thisbaseexp = pc_thisbaseexp;
	pc->nextjobexp = pc_nextjobexp;
	pc->thisjobexp = pc_thisjobexp;
	pc->gets_status_point = pc_gets_status_point;
	pc->need_status_point = pc_need_status_point;
	pc->maxparameterincrease = pc_maxparameterincrease;
	pc->statusup = pc_statusup;
	pc->statusup2 = pc_statusup2;
	pc->skillup = pc_skillup;
	pc->allskillup = pc_allskillup;
	pc->resetlvl = pc_resetlvl;
	pc->resetstate = pc_resetstate;
	pc->resetskill = pc_resetskill;
	pc->resetskill_job = pc_resetskill_job;
	pc->resetfeel = pc_resetfeel;
	pc->resethate = pc_resethate;
	pc->equipitem = pc_equipitem;
	pc->equipitem_pos = pc_equipitem_pos;
	pc->unequipitem = pc_unequipitem;
	pc->unequipitem_pos = pc_unequipitem_pos;
	pc->checkitem = pc_checkitem;
	pc->useitem = pc_useitem;
	pc->autocast_clear_current = pc_autocast_clear_current;
	pc->autocast_clear = pc_autocast_clear;
	pc->autocast_set_current = pc_autocast_set_current;
	pc->autocast_remove = pc_autocast_remove;

	pc->skillatk_bonus = pc_skillatk_bonus;
	pc->sub_skillatk_bonus = pc_sub_skillatk_bonus;
	pc->skillheal_bonus = pc_skillheal_bonus;
	pc->skillheal2_bonus = pc_skillheal2_bonus;

	pc->damage = pc_damage;
	pc->dead = pc_dead;
	pc->revive = pc_revive;
	pc->heal = pc_heal;
	pc->itemheal = pc_itemheal;
	pc->percentheal = pc_percentheal;
	pc->jobchange = pc_jobchange;
	pc->hide = pc_hide;
	pc->unhide = pc_unhide;
	pc->setoption = pc_setoption;
	pc->setcart = pc_setcart;
	pc->setfalcon = pc_setfalcon;
	pc->setridingpeco = pc_setridingpeco;
	pc->setmadogear = pc_setmadogear;
	pc->setridingdragon = pc_setridingdragon;
	pc->setridingwug = pc_setridingwug;
	pc->changelook = pc_changelook;
	pc->equiplookall = pc_equiplookall;

	pc->readparam = pc_readparam;
	pc->setparam = pc_setparam;
	pc->readreg = pc_readreg;
	pc->setreg = pc_setreg;
	pc->readregstr = pc_readregstr;
	pc->setregstr = pc_setregstr;
	pc->readregistry = pc_readregistry;
	pc->setregistry = pc_setregistry;
	pc->readregistry_str = pc_readregistry_str;
	pc->setregistry_str = pc_setregistry_str;

	pc->addeventtimer = pc_addeventtimer;
	pc->deleventtimer = pc_deleventtimer;
	pc->cleareventtimer = pc_cleareventtimer;
	pc->addeventtimercount = pc_addeventtimercount;

	pc->calc_pvprank_sub = pc_calc_pvprank_sub;
	pc->calc_pvprank = pc_calc_pvprank;
	pc->calc_pvprank_timer = pc_calc_pvprank_timer;

	pc->ismarried = pc_ismarried;
	pc->marriage = pc_marriage;
	pc->divorce = pc_divorce;
	pc->get_partner = pc_get_partner;
	pc->get_father = pc_get_father;
	pc->get_mother = pc_get_mother;
	pc->get_child = pc_get_child;

	pc->bleeding = pc_bleeding;
	pc->regen = pc_regen;

	pc->setstand = pc_setstand;
	pc->candrop = pc_candrop;
	pc->can_talk = pc_can_talk;
	pc->can_attack = pc_can_attack;

	pc->jobid2mapid = pc_jobid2mapid; // Skotlex
	pc->mapid2jobid = pc_mapid2jobid; // Skotlex

	pc->job_name = pc_job_name;

	pc->setinvincibletimer = pc_setinvincibletimer;
	pc->delinvincibletimer = pc_delinvincibletimer;

	pc->addspiritball = pc_addspiritball;
	pc->addspiritball_sub = pc_addspiritball_sub;
	pc->delspiritball = pc_delspiritball;
	pc->delspiritball_sub = pc_delspiritball_sub;
	pc->addsoulball = pc_addsoulball;
	pc->delsoulball = pc_delsoulball;
	pc->addfame = pc_addfame;
	pc->fame_rank = pc_fame_rank;
	pc->famelist_type = pc_famelist_type;
	pc->set_hate_mob = pc_set_hate_mob;
	pc->getmaxspiritball = pc_getmaxspiritball;

	pc->readdb = pc_readdb;
	pc->read_exp_db = pc_read_exp_db;
	pc->read_exp_db_sub = pc_read_exp_db_sub;
	pc->read_exp_db_sub_class = pc_read_exp_db_sub_class;
	pc->read_attr_fix_db = pc_read_attr_fix_db;
	pc->read_attr_fix_db_entry = pc_read_attr_fix_db_entry;
	pc->read_attr_fix_db_level = pc_read_attr_fix_db_level;
	pc->map_day_timer = map_day_timer; // by [yor]
	pc->map_night_timer = map_night_timer; // by [yor]
	// Rental System
	pc->inventory_rentals = pc_inventory_rentals;
	pc->inventory_rental_clear = pc_inventory_rental_clear;
	pc->inventory_rental_add = pc_inventory_rental_add;

	pc->disguise = pc_disguise;
	pc->isautolooting = pc_isautolooting;

	pc->overheat = pc_overheat;
	pc->banding = pc_banding;

	pc->itemcd_do = pc_itemcd_do;
	pc->load_combo = pc_load_combo;

	pc->add_charm = pc_add_charm;
	pc->del_charm = pc_del_charm;

	pc->baselevelchanged = pc_baselevelchanged;
	pc->level_penalty_mod = pc_level_penalty_mod;

	pc->calc_skillpoint = pc_calc_skillpoint;

	pc->invincible_timer = pc_invincible_timer;
	pc->spiritball_timer = pc_spiritball_timer;
	pc->check_banding = pc_check_banding;
	pc->inventory_rental_end = pc_inventory_rental_end;
	pc->check_skilltree = pc_check_skilltree;
	pc->bonus_autospell = pc_bonus_autospell;
	pc->bonus_autospell_onskill = pc_bonus_autospell_onskill;
	pc->bonus_addeff = pc_bonus_addeff;
	pc->bonus_addeff_onskill = pc_bonus_addeff_onskill;
	pc->bonus_item_drop = pc_bonus_item_drop;
	pc->calcexp = pc_calcexp;
	pc->respawn_timer = pc_respawn_timer;
	pc->jobchange_killclone = jobchange_killclone;
	pc->getstat = pc_getstat;
	pc->setstat = pc_setstat;
	pc->eventtimer = pc_eventtimer;
	pc->daynight_timer_sub = pc_daynight_timer_sub;
	pc->charm_timer = pc_charm_timer;
	pc->readdb_levelpenalty = pc_readdb_levelpenalty;
	pc->autosave = pc_autosave;
	pc->follow_timer = pc_follow_timer;
	pc->read_skill_tree = pc_read_skill_tree;
	pc->read_skill_job_skip = pc_read_skill_job_skip;
	pc->clear_skill_tree = pc_clear_skill_tree;
	pc->isUseitem = pc_isUseitem;
	pc->show_steal = pc_show_steal;
	pc->checkcombo = pc_checkcombo;
	pc->calcweapontype = pc_calcweapontype;
	pc->removecombo = pc_removecombo;
	pc->update_job_and_level = pc_update_job_and_level;
	pc->clear_exp_groups = pc_clear_exp_groups;
	pc->init_exp_groups = pc_init_exp_groups;
	pc->job_is_dummy = pc_job_is_dummy;

	pc->bank_withdraw = pc_bank_withdraw;
	pc->bank_deposit = pc_bank_deposit;

	pc->rental_expire = pc_rental_expire;
	pc->scdata_received = pc_scdata_received;

	pc->bound_clear = pc_bound_clear;

	pc->expiration_timer = pc_expiration_timer;
	pc->global_expiration_timer = pc_global_expiration_timer;
	pc->expire_check = pc_expire_check;
	pc->db_checkid = pc_db_checkid;
	pc->validate_levels = pc_validate_levels;

	pc->check_supernovice_call = pc_check_supernovice_call;
	pc->process_chat_message = pc_process_chat_message;
	pc->wis_message_to_gm = pc_wis_message_to_gm;
	pc->wis_message_to_gm_sub = pc_wis_message_to_gm_sub;

	/**
	 * Autotrade persistency [Ind/Hercules <3]
	 **/
	pc->autotrade_load = pc_autotrade_load;
	pc->autotrade_update = pc_autotrade_update;
	pc->autotrade_start = pc_autotrade_start;
	pc->autotrade_prepare = pc_autotrade_prepare;
	pc->autotrade_populate = pc_autotrade_populate;
	pc->autotrade_final = pc_autotrade_final;

	pc->check_job_name = pc_check_job_name;
	pc->update_idle_time = pc_update_idle_time;

	pc->have_magnifier = pc_have_magnifier;
	pc->have_item_chain = pc_have_item_chain;

	pc->check_basicskill = pc_check_basicskill;

	pc->isDeathPenaltyJob = pc_isDeathPenaltyJob;
	pc->has_second_costume = pc_has_second_costume;
	pc->expandInventory = pc_expandInventory;
	pc->auto_exp_insurance = pc_auto_exp_insurance;

	pc->crimson_marker_clear = pc_crimson_marker_clear;
}
