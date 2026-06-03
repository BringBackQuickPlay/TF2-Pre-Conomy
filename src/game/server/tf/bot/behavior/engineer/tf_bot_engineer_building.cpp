//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_building.cpp
// At building location, constructing buildings
// Michael Booth, May 2010

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_player.h"
#include "tf_obj.h"
#include "tf_obj_sentrygun.h"
#include "tf_obj_dispenser.h"
#include "tf_obj_teleporter.h"
#include "tf_gamerules.h"
#include "tf_weapon_builder.h"
#include "team_train_watcher.h"
#include "bot/tf_bot.h"
#include "bot/behavior/engineer/tf_bot_engineer_building.h"
#include "bot/behavior/engineer/tf_bot_engineer_move_to_build.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_teleport_exit.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_sentrygun.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_dispenser.h"
#include "bot/behavior/engineer/tf_bot_engineer_assist_friendly_building.h"
#include "bot/behavior/engineer/tf_bot_engineer_patrol_nest.h"
#include "bot/behavior/tf_bot_attack.h"
#include "bot/behavior/tf_bot_get_ammo.h"
#include "bot/map_entities/tf_bot_hint_teleporter_exit.h"
#include "bot/map_entities/tf_bot_hint_sentrygun.h"
#include "NextBotUtil.h"


ConVar tf_bot_engineer_retaliate_range( "tf_bot_engineer_retaliate_range", "750", FCVAR_CHEAT, "If attacker who destroyed sentry is closer than this, attack. Otherwise, retreat" );
ConVar tf_bot_engineer_exit_near_sentry_range( "tf_bot_engineer_exit_near_sentry_range", "2500", FCVAR_CHEAT, "Maximum travel distance between a bot's Sentry gun and its Teleporter Exit" );
ConVar tf_bot_engineer_max_sentry_travel_distance_to_point( "tf_bot_engineer_max_sentry_travel_distance_to_point", "2500", FCVAR_CHEAT, "Maximum travel distance between a bot's Sentry gun and the currently contested point" );
ConVar tf_bot_engineer_improved_behavior( "tf_bot_engineer_improved_behavior", "0", FCVAR_CHEAT, "If set, Engineer bots do not treat mini-sentries as upgradeable sentries and should not get stuck trying to upgrade sentry/stuck not upgrading dispenser." );


extern ConVar tf_bot_path_lookahead_range;

const int MaxPlacementAttempts = 5;


//---------------------------------------------------------------------------------------------
CTFBotEngineerBuilding::CTFBotEngineerBuilding( void )
{
	m_sentryBuildHint = NULL;
}


//---------------------------------------------------------------------------------------------
CTFBotEngineerBuilding::CTFBotEngineerBuilding( CTFBotHintSentrygun *sentryBuildHint )
{
	m_sentryBuildHint = sentryBuildHint;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotEngineerBuilding::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_sentryTriesLeft = MaxPlacementAttempts;

	m_territoryRangeTimer.Invalidate();

	m_hasBuiltSentry = false;
	m_isSentryOutOfPosition = false;
	m_nearbyMetalStatus = NEARBY_METAL_UNKNOWN;

	m_assistFriendlyBuildingTimer.Start( RandomFloat( 3.0f, 6.0f ) );
	m_patrolNestTimer.Start( RandomFloat( 10.0f, 20.0f ) );
	m_noFriendlyPatrolTargetTimer.Start( 30.0f );

	for ( int i = 0; i < ARRAYSIZE( m_friendlySentryMemory ); ++i )
	{
		m_friendlySentryMemory[i].sentry = NULL;
		m_friendlySentryMemory[i].builder = NULL;
		m_friendlySentryMemory[i].lastKnownPosition = vec3_origin;
		m_friendlySentryMemory[i].forgetTimer.Invalidate();
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
// Attack the current target of our mini-sentry, but do not chase too far away from it.
bool CTFBotEngineerBuilding::TryAttackMiniSentryTarget( CTFBot *me, CObjectSentrygun *mySentry )
{
	if ( !me || !mySentry || !mySentry->IsMiniBuilding() )
		return false;

	CBaseEntity *targetEntity = mySentry->GetTarget();
	CTFPlayer *targetPlayer = ToTFPlayer( targetEntity );

	if ( !targetPlayer || !targetPlayer->IsAlive() )
		return false;

	if ( targetPlayer->GetTeamNumber() == me->GetTeamNumber() )
		return false;

	const float assistRange = 512.0f;
	Vector toTarget = targetPlayer->WorldSpaceCenter() - me->WorldSpaceCenter();

	if ( toTarget.LengthSqr() > assistRange * assistRange )
		return false;

	const float sentryLeashRange = 150.0f;
	float rangeToSentry = me->GetDistanceBetween( mySentry );

	if ( rangeToSentry > sentryLeashRange )
	{
		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 0.5f, 1.0f ) );

			CTFBotPathCost cost( me, FASTEST_ROUTE );
			m_path.Compute( me, mySentry->GetAbsOrigin(), cost );
		}

		m_path.Update( me );
		return true;
	}

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( !shotgun )
		return false;

	me->Weapon_Switch( shotgun );
	me->StopLookingAroundForEnemies();
	me->GetBodyInterface()->AimHeadTowards( targetPlayer->WorldSpaceCenter(), IBody::CRITICAL, 0.5f, NULL, "Attack my mini-sentry target" );
	me->PressFireButton();

	return true;
}



//---------------------------------------------------------------------------------------------
static bool IsBuildingImportantToMeUnderThreat( CTFBot *me, CBaseObject *obj, float recentTime )
{
	if ( !obj )
		return false;

	if ( obj->HasSapper() || obj->IsPlasmaDisabled() )
		return true;

	if ( obj->GetTimeSinceLastInjury() < recentTime )
		return true;

	if ( obj->GetHealth() < obj->GetMaxHealth() )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
static bool IsFriendlyObjectNotMine( CTFBot *me, CBaseObject *obj )
{
	if ( !me || !obj )
		return false;

	if ( obj->GetTeamNumber() != me->GetTeamNumber() )
		return false;

	if ( obj->GetBuilder() == me )
		return false;

	return true;
}


//---------------------------------------------------------------------------------------------
static bool IsVisibleBuildingForEngineer( CTFBot *me, CBaseObject *obj )
{
	if ( !me || !obj )
		return false;

	Vector eye = me->EyePosition();
	Vector center = obj->WorldSpaceCenter();

	Vector toBuilding = center - eye;
	toBuilding.z = 0.0f;

	Vector side( -toBuilding.y, toBuilding.x, 0.0f );
	if ( side.NormalizeInPlace() <= 0.0f )
	{
		side = Vector( 1.0f, 0.0f, 0.0f );
	}

	Vector tracePoints[] =
	{
		center,
		center + Vector( 0.0f, 0.0f, 32.0f ),
		center + side * 24.0f,
		center - side * 24.0f,
		center + side * 24.0f + Vector( 0.0f, 0.0f, 32.0f ),
		center - side * 24.0f + Vector( 0.0f, 0.0f, 32.0f )
	};

	for ( int i = 0; i < ARRAYSIZE( tracePoints ); ++i )
	{
		trace_t result;
		UTIL_TraceLine( eye, tracePoints[i], MASK_SOLID_BRUSHONLY, me, COLLISION_GROUP_NONE, &result );

		if ( result.fraction >= 1.0f )
			return true;

		if ( result.m_pEnt == obj )
			return true;
	}

	return false;
}


//---------------------------------------------------------------------------------------------
static bool IsFriendlyBuildingAssistCandidate( CTFBot *me, CBaseObject *obj )
{
	if ( !IsFriendlyObjectNotMine( me, obj ) )
		return false;

	if ( !IsVisibleBuildingForEngineer( me, obj ) )
		return false;

	if ( FClassnameIs( obj, "obj_sentrygun" ) )
	{
		CObjectSentrygun *sentry = (CObjectSentrygun *)obj;
		if ( sentry->IsMiniBuilding() )
			return false;
	}

	if ( obj->HasSapper() || obj->IsPlasmaDisabled() )
		return true;

	if ( obj->GetHealth() < obj->GetMaxHealth() )
		return true;

	if ( obj->IsBuilding() )
		return true;

	if ( obj->GetUpgradeLevel() < 3 )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::ShouldConsiderFriendlyEngineerActions( CTFBot *me ) const
{
	if ( !me )
		return false;

	if ( me->GetTimeSinceLastInjury() < 1.0f )
		return false;

	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );

	if ( !mySentry || !myDispenser )
		return false;

	if ( IsBuildingImportantToMeUnderThreat( me, myDispenser, 8.0f ) )
		return false;

	if ( myDispenser->IsBuilding() )
		return false;

	if ( mySentry->IsMiniBuilding() )
		return true;

	if ( IsBuildingImportantToMeUnderThreat( me, mySentry, 8.0f ) )
		return false;

	if ( mySentry->IsBuilding() )
		return false;

	if ( mySentry->GetUpgradeLevel() < 3 )
		return false;

	return true;
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::ClearExpiredFriendlySentryMemory( void )
{
	for ( int i = 0; i < ARRAYSIZE( m_friendlySentryMemory ); ++i )
	{
		CObjectSentrygun *sentry = m_friendlySentryMemory[i].sentry.Get();

		if ( !m_friendlySentryMemory[i].forgetTimer.HasStarted() || m_friendlySentryMemory[i].forgetTimer.IsElapsed() || !sentry )
		{
			m_friendlySentryMemory[i].sentry = NULL;
			m_friendlySentryMemory[i].builder = NULL;
			m_friendlySentryMemory[i].lastKnownPosition = vec3_origin;
			m_friendlySentryMemory[i].forgetTimer.Invalidate();
		}
	}
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::RememberFriendlySentry( CTFBot *me, CObjectSentrygun *sentry )
{
	if ( !me || !sentry )
		return;

	if ( !IsFriendlyObjectNotMine( me, sentry ) )
		return;

	if ( sentry->IsMiniBuilding() )
		return;

	CTFPlayer *builder = ToTFPlayer( sentry->GetBuilder() );

	int freeSlot = -1;
	int replaceSlot = 0;
	float farthestRememberedRange = -1.0f;

	for ( int i = 0; i < ARRAYSIZE( m_friendlySentryMemory ); ++i )
	{
		CObjectSentrygun *remembered = m_friendlySentryMemory[i].sentry.Get();

		if ( remembered == sentry || ( builder && m_friendlySentryMemory[i].builder.Get() == builder ) )
		{
			m_friendlySentryMemory[i].sentry = sentry;
			m_friendlySentryMemory[i].builder = builder;
			m_friendlySentryMemory[i].lastKnownPosition = sentry->GetAbsOrigin();
			m_friendlySentryMemory[i].forgetTimer.Start( 30.0f );
			return;
		}

		if ( !remembered && freeSlot < 0 )
		{
			freeSlot = i;
		}

		if ( remembered )
		{
			float range = me->GetDistanceBetween( remembered );
			if ( range > farthestRememberedRange )
			{
				farthestRememberedRange = range;
				replaceSlot = i;
			}
		}
	}

	int slot = ( freeSlot >= 0 ) ? freeSlot : replaceSlot;
	m_friendlySentryMemory[slot].sentry = sentry;
	m_friendlySentryMemory[slot].builder = builder;
	m_friendlySentryMemory[slot].lastKnownPosition = sentry->GetAbsOrigin();
	m_friendlySentryMemory[slot].forgetTimer.Start( 30.0f );
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::UpdateFriendlySentryMemory( CTFBot *me )
{
	ClearExpiredFriendlySentryMemory();

	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "obj_sentrygun" ) ) != NULL )
	{
		CObjectSentrygun *sentry = dynamic_cast< CObjectSentrygun * >( ent );
		if ( !sentry )
			continue;

		if ( !IsFriendlyObjectNotMine( me, sentry ) )
			continue;

		if ( sentry->IsMiniBuilding() )
			continue;

		if ( !IsVisibleBuildingForEngineer( me, sentry ) )
			continue;

		RememberFriendlySentry( me, sentry );
	}
}


//---------------------------------------------------------------------------------------------
CObjectSentrygun *CTFBotEngineerBuilding::FindRememberedFriendlySentry( CTFBot *me ) const
{
	CObjectSentrygun *bestSentry = NULL;
	float bestRange = FLT_MAX;

	for ( int i = 0; i < ARRAYSIZE( m_friendlySentryMemory ); ++i )
	{
		CObjectSentrygun *sentry = m_friendlySentryMemory[i].sentry.Get();

		if ( !sentry )
			continue;

		if ( !m_friendlySentryMemory[i].forgetTimer.HasStarted() || m_friendlySentryMemory[i].forgetTimer.IsElapsed() )
			continue;

		if ( !IsFriendlyObjectNotMine( me, sentry ) )
			continue;

		if ( sentry->IsMiniBuilding() )
			continue;

		float range = me->GetDistanceBetween( sentry );
		if ( range < bestRange )
		{
			bestSentry = sentry;
			bestRange = range;
		}
	}

	return bestSentry;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerBuilding::FindFriendlyBuildingAssistTarget( CTFBot *me ) const
{
	CBaseObject *bestDamagedSentry = NULL;
	CBaseObject *bestUpgradeSentry = NULL;
	CBaseObject *bestDamagedOther = NULL;
	CBaseObject *bestUpgradeOther = NULL;

	float bestDamagedSentryRange = FLT_MAX;
	float bestUpgradeSentryRange = FLT_MAX;
	float bestDamagedOtherRange = FLT_MAX;
	float bestUpgradeOtherRange = FLT_MAX;

	const char *classNames[] =
	{
		"obj_sentrygun",
		"obj_dispenser",
		"obj_teleporter"
	};

	for ( int whichClass = 0; whichClass < ARRAYSIZE( classNames ); ++whichClass )
	{
		CBaseEntity *ent = NULL;
		while ( ( ent = gEntList.FindEntityByClassname( ent, classNames[ whichClass ] ) ) != NULL )
		{
			CBaseObject *obj = dynamic_cast< CBaseObject * >( ent );
			if ( !obj )
				continue;

			if ( !IsFriendlyBuildingAssistCandidate( me, obj ) )
				continue;

			float range = me->GetDistanceBetween( obj );

			if ( FClassnameIs( obj, "obj_sentrygun" ) )
			{
				if ( obj->HasSapper() || obj->IsPlasmaDisabled() || obj->GetHealth() < obj->GetMaxHealth() || obj->GetTimeSinceLastInjury() < 1.0f )
				{
					if ( range < bestDamagedSentryRange )
					{
						bestDamagedSentry = obj;
						bestDamagedSentryRange = range;
					}
				}
				else if ( obj->GetUpgradeLevel() < 3 )
				{
					if ( range < bestUpgradeSentryRange )
					{
						bestUpgradeSentry = obj;
						bestUpgradeSentryRange = range;
					}
				}
			}
			else
			{
				if ( obj->HasSapper() || obj->IsPlasmaDisabled() || obj->GetHealth() < obj->GetMaxHealth() || obj->GetTimeSinceLastInjury() < 1.0f )
				{
					if ( range < bestDamagedOtherRange )
					{
						bestDamagedOther = obj;
						bestDamagedOtherRange = range;
					}
				}
				else if ( obj->GetUpgradeLevel() < 3 )
				{
					if ( range < bestUpgradeOtherRange )
					{
						bestUpgradeOther = obj;
						bestUpgradeOtherRange = range;
					}
				}
			}
		}
	}

	if ( bestDamagedSentry )
		return bestDamagedSentry;

	if ( bestUpgradeSentry )
		return bestUpgradeSentry;

	if ( bestDamagedOther )
		return bestDamagedOther;

	return bestUpgradeOther;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerBuilding::FindFriendlyNestPatrolTarget( CTFBot *me ) const
{
	CBaseObject *bestSentry = NULL;
	CBaseObject *bestDispenser = NULL;
	float bestSentryRange = FLT_MAX;
	float bestDispenserRange = FLT_MAX;

	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "obj_sentrygun" ) ) != NULL )
	{
		CObjectSentrygun *sentry = dynamic_cast< CObjectSentrygun * >( ent );
		if ( !sentry )
			continue;

		if ( !IsFriendlyObjectNotMine( me, sentry ) )
			continue;

		if ( sentry->IsMiniBuilding() )
			continue;

		if ( !IsVisibleBuildingForEngineer( me, sentry ) )
			continue;

		float range = me->GetDistanceBetween( sentry );
		if ( range < bestSentryRange )
		{
			bestSentry = sentry;
			bestSentryRange = range;
		}
	}

	ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "obj_dispenser" ) ) != NULL )
	{
		CObjectDispenser *dispenser = dynamic_cast< CObjectDispenser * >( ent );
		if ( !dispenser )
			continue;

		if ( !IsFriendlyObjectNotMine( me, dispenser ) )
			continue;

		if ( !IsVisibleBuildingForEngineer( me, dispenser ) )
			continue;

		float range = me->GetDistanceBetween( dispenser );
		if ( range < bestDispenserRange )
		{
			bestDispenser = dispenser;
			bestDispenserRange = range;
		}
	}

	if ( bestSentry && bestDispenser )
	{
		if ( RandomInt( 1, 100 ) <= 25 )
			return bestSentry;

		return bestDispenser;
	}

	if ( bestSentry )
		return bestSentry;

	if ( bestDispenser )
		return bestDispenser;

	return FindRememberedFriendlySentry( me );
}


//---------------------------------------------------------------------------------------------
// Everything is built, upgrade/maintain it
// TODO: Upgrade/maintain nearby friendly buildings, too.
void CTFBotEngineerBuilding::UpgradeAndMaintainBuildings( CTFBot *me )
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );

	if ( !mySentry )
	{
		return;
	}

	const float tooFarRange = 75.0f;

	if ( !myDispenser )
	{
		// just work on our sentry
		float rangeToSentry = me->GetDistanceBetween( mySentry );

		if ( rangeToSentry < 1.2f * tooFarRange )
		{
			// crouch both for cover behind our buildings, but also to slow us down so we hit our move goal more accurately
			me->PressCrouchButton();
		}

		if ( rangeToSentry > tooFarRange )
		{
			if ( m_repathTimer.IsElapsed() )
			{
				m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, mySentry->GetAbsOrigin(), cost );
			}

			m_path.Update( me );
		}
		else
		{
			// we are in position - work on our buildings
			CBaseCombatWeapon *wrench = me->Weapon_GetSlot( TF_WPN_TYPE_MELEE );
			if ( wrench )
			{
				me->Weapon_Switch( wrench );
			}

			me->StopLookingAroundForEnemies();
			me->GetBodyInterface()->AimHeadTowards( mySentry->WorldSpaceCenter(), IBody::CRITICAL, 1.0f, NULL, "Work on my Sentry" );
			me->PressFireButton();
		}

		return;
	}

	// sit near both buildings
	Vector betweenMyBuildings = ( mySentry->GetAbsOrigin() + myDispenser->GetAbsOrigin() ) / 2.0f;

	// try to equalize distance between both
	float rangeToSentry = me->GetDistanceBetween( mySentry );
	float rangeToDispenser = me->GetDistanceBetween( myDispenser );

	const float equalTolerance = 25.0f;

	if ( rangeToSentry < 1.2f * tooFarRange && rangeToDispenser < 1.2f * tooFarRange )
	{
		// crouch both for cover behind our buildings, but also to slow us down so we hit our move goal more accurately
		me->PressCrouchButton();
	}

	if ( fabs( rangeToDispenser - rangeToSentry ) > equalTolerance || rangeToSentry > tooFarRange || rangeToDispenser > tooFarRange )
	{
		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

			CTFBotPathCost cost( me, FASTEST_ROUTE );
			m_path.Compute( me, betweenMyBuildings, cost );
		}

		m_path.Update( me );
	}

	if ( rangeToSentry < tooFarRange || rangeToDispenser < tooFarRange )
	{
		// we are (nearly) in position - work on our buildings
		m_searchTimer.Invalidate();

		bool bImprovedMiniSentry = tf_bot_engineer_improved_behavior.GetBool() && mySentry->IsMiniBuilding();



		CBaseObject *workTarget = bImprovedMiniSentry ? NULL : mySentry;

		int iSentryLevel = mySentry->GetUpgradeLevel();
		bool bUpgradeSentry = iSentryLevel < 3;
		bool bRepairSentry = mySentry->GetTimeSinceLastInjury() < 1.0f || mySentry->GetHealth() < mySentry->GetMaxHealth();
		bool bSentryNeedsAmmo = false;
		int iDesiredDispenserLevel = iSentryLevel;

		if ( bImprovedMiniSentry )
		{
			bUpgradeSentry = false;
			bRepairSentry = false;
			bSentryNeedsAmmo = mySentry->IsAmmoLow( 0.50f );
			iDesiredDispenserLevel = 3;

			// If bImprovedMiniSentry, stand up again as there's little point to crouch behind a mini-sentry.
			me->ReleaseCrouchButton();
		}

		if ( mySentry->HasSapper() || mySentry->IsPlasmaDisabled() )
			workTarget = mySentry;
		else if ( myDispenser->HasSapper() || myDispenser->IsPlasmaDisabled() )
			workTarget = myDispenser;
		else if ( bRepairSentry )
			workTarget = mySentry;
		else if ( mySentry->IsBuilding() )
			workTarget = mySentry;
		else if ( myDispenser->IsBuilding() )
			workTarget = myDispenser;
		else if ( bUpgradeSentry )
			workTarget = mySentry;
		else if ( myDispenser->GetHealth() < myDispenser->GetMaxHealth() )
			workTarget = myDispenser;
		else if ( bSentryNeedsAmmo )
			workTarget = mySentry;
		else if ( myDispenser->GetUpgradeLevel() < iDesiredDispenserLevel )
			workTarget = myDispenser;

		if ( workTarget )
		{
			CBaseCombatWeapon *wrench = me->Weapon_GetSlot( TF_WPN_TYPE_MELEE );
			if ( wrench )
			{
				me->Weapon_Switch( wrench );
			}
			me->StopLookingAroundForEnemies();
			me->GetBodyInterface()->AimHeadTowards( workTarget->WorldSpaceCenter(), IBody::CRITICAL, 1.0f, NULL, "Work on my buildings" );
			me->PressFireButton();
		} 
		else
		{
			// We did not have a workTarget, consider checking if our
			// mini-sentry has a target it's attacking and if it's close.
			// if true, then attack the target of our mini-sentry.
			if ( bImprovedMiniSentry && TryAttackMiniSentryTarget( me, mySentry ) )
			{
				return;
			}

			// 
			CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
			if ( shotgun )
			{
				me->Weapon_Switch( shotgun );
			}

			me->StartLookingAroundForEnemies();

			// Any friendly nearby buildings we can help?
		}
	}
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::IsMetalSourceNearby( CTFBot *me ) const
{
	CUtlVector< CNavArea * > nearbyVector;
	CollectSurroundingAreas( &nearbyVector, me->GetLastKnownArea(), 2000.0f, me->GetLocomotionInterface()->GetStepHeight(), me->GetLocomotionInterface()->GetStepHeight() );

	for( int i=0; i<nearbyVector.Count(); ++i )
	{
		CTFNavArea *area = (CTFNavArea *)nearbyVector[i];
		if ( area->HasAttributeTF( TF_NAV_HAS_AMMO ) )
		{
			return true;
		}

		// this assumes all spawn rooms have resupply cabinets
		if ( me->GetTeamNumber() == TF_TEAM_RED && area->HasAttributeTF( TF_NAV_SPAWN_ROOM_RED ) )
		{
			return true;
		}

		if ( me->GetTeamNumber() == TF_TEAM_BLUE && area->HasAttributeTF( TF_NAV_SPAWN_ROOM_BLUE ) )
		{
			return true;
		}
	}

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::CheckIfSentryIsOutOfPosition( CTFBot *me ) const
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );

	if ( !mySentry )
	{
		return false;
	}

	// payload
	if ( TFGameRules()->GetGameType() == TF_GAMETYPE_ESCORT )
	{
		CTeamTrainWatcher *trainWatcher;

		if ( me->GetTeamNumber() == TF_TEAM_BLUE )
		{
			trainWatcher = TFGameRules()->GetPayloadToPush( me->GetTeamNumber() );
		}
		else
		{
			trainWatcher = TFGameRules()->GetPayloadToBlock( me->GetTeamNumber() );
		}

		if ( trainWatcher )
		{
			float sentryDistanceAlongPath;
			trainWatcher->ProjectPointOntoPath( mySentry->GetAbsOrigin(), NULL, &sentryDistanceAlongPath );

			const float behindTrainTolerance = SENTRY_MAX_RANGE;
			return ( trainWatcher->GetTrainDistanceAlongTrack() > sentryDistanceAlongPath + behindTrainTolerance );
		}
	}

	// control points
	mySentry->UpdateLastKnownArea();
	CNavArea *sentryArea = mySentry->GetLastKnownArea();

	CTeamControlPoint *point = me->GetMyControlPoint();
	if ( point )
	{
		CTFNavArea *pointArea = TheTFNavMesh()->GetControlPointCenterArea( point->GetPointIndex() );

		if ( sentryArea && pointArea )
		{
			CTFBotPathCost cost( me, FASTEST_ROUTE );
			if ( NavAreaTravelDistance( sentryArea, pointArea, cost, tf_bot_engineer_max_sentry_travel_distance_to_point.GetFloat() ) < 0 &&
				 NavAreaTravelDistance( pointArea, sentryArea, cost, tf_bot_engineer_max_sentry_travel_distance_to_point.GetFloat() ) < 0 )
			{
				return true;
			}
		}
	}

	return false;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotEngineerBuilding::Update( CTFBot *me, float interval )
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );
	CObjectTeleporter *myTeleportEntrance = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_ENTRANCE );
	CObjectTeleporter *myTeleportExit = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_EXIT );

	bool isUnderAttack = ( me->GetTimeSinceLastInjury() < 1.0f );
	isUnderAttack |= ( mySentry && ( mySentry->HasSapper() || mySentry->IsPlasmaDisabled() ) );
	isUnderAttack |= ( myDispenser && ( myDispenser->HasSapper() || myDispenser->IsPlasmaDisabled() ) );

	me->StartLookingAroundForEnemies();

	// try to build a Sentry
	if ( !mySentry )
	{
		m_nearbyMetalStatus = NEARBY_METAL_UNKNOWN;

		// react to nearby threats if our sentry is down
		const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
		if ( threat && threat->IsVisibleRecently() )
		{
			me->EquipBestWeaponForThreat( threat );
		}

		if ( !m_hasBuiltSentry && m_sentryTriesLeft > 0 )
		{
			--m_sentryTriesLeft;

			if ( m_sentryBuildHint )
			{
				return SuspendFor( new CTFBotEngineerBuildSentryGun( m_sentryBuildHint ), "Building a Sentry at a hint location" );
			}

			return SuspendFor( new CTFBotEngineerBuildSentryGun, "Building a Sentry" );
		}
		else
		{
			// can't build a Sentry here - pick a new place
			return ChangeTo( new CTFBotEngineerMoveToBuild, "Couldn't find a place to build" );
		}
	}

	// I have a Sentry
	m_hasBuiltSentry = true;

	if ( m_sentryBuildHint != NULL && !m_sentryBuildHint->IsEnabled() )
	{
		// our hint has been disabled and no longer has influence on our behavior
		m_sentryBuildHint = NULL;
	}

	// periodically check that our Sentry is still near the contested point
	if ( m_sentryBuildHint == NULL || !m_sentryBuildHint->IsSticky() )
	{
		if ( !m_isSentryOutOfPosition && m_territoryRangeTimer.IsElapsed() )
		{
			m_territoryRangeTimer.Start( RandomFloat( 3.0f, 5.0f ) );

			m_isSentryOutOfPosition = CheckIfSentryIsOutOfPosition( me );
		}

		if ( m_isSentryOutOfPosition )
		{
			// the point has moved, only keep sentry as long as it keeps attacking
			if ( mySentry->GetTimeSinceLastFired() > 10.0f )
			{
				mySentry->DetonateObject();

				// if we built here because of a hint, disable that hint so we don't use it and rebuild here again
				if ( m_sentryBuildHint != NULL )
				{
					inputdata_t dummy;
					m_sentryBuildHint->InputDisable( dummy );

					m_sentryBuildHint = NULL;
				}

				if ( myDispenser )
				{
					myDispenser->DetonateObject();
				}

				if ( myTeleportExit )
				{
					myTeleportExit->DetonateObject();
				}

				me->SpeakConceptIfAllowed( MP_CONCEPT_PLAYER_MOVEUP );

				return ChangeTo( new CTFBotEngineerMoveToBuild, "Need to move my gear closer to the point!" );
			}
		}
	}

	// if my dispenser is too far away from my sentry, destroy and rebuild it next update
	// @TODO: Flag hint-built entities for a larger range
	const float maxSeparation = 500.0f;
	if ( myDispenser )
	{
		if ( ( mySentry->GetAbsOrigin() - myDispenser->GetAbsOrigin() ).IsLengthGreaterThan( maxSeparation ) )
		{
			myDispenser->DestroyObject();
			myDispenser = NULL;
		}
	}

	// build up the sentry all the way if there is a metal source nearby
	if ( mySentry->GetUpgradeLevel() < 3 && ( !tf_bot_engineer_improved_behavior.GetBool() || !mySentry->IsMiniBuilding() ) )
	{
		if ( m_nearbyMetalStatus == NEARBY_METAL_UNKNOWN )
		{
			m_nearbyMetalStatus = IsMetalSourceNearby( me ) ? NEARBY_METAL_EXISTS : NEARBY_METAL_NONE;
		}

		if ( m_nearbyMetalStatus == NEARBY_METAL_EXISTS )
		{
			UpgradeAndMaintainBuildings( me );
			return Continue();
		}
	}

/*
	if ( myTeleportExit )
	{
		// if my teleporter exit is too far away from my sentry, destroy and rebuild it next update
		if ( ( mySentry->GetAbsOrigin() - myTeleportExit->GetAbsOrigin() ).IsLengthGreaterThan( maxSeparation ) )
		{
			myTeleportExit->DestroyObject();
			myTeleportExit = NULL;
		}
	}
*/

	// try to build a Dispenser
	const float dispenserRebuildInterval = 10.0f;
	if ( myDispenser )
	{
		// don't rebuild immediately after building is destroyed
		m_dispenserRetryTimer.Start( dispenserRebuildInterval );
	}
	else if ( m_dispenserRetryTimer.IsElapsed() && !isUnderAttack )
	{
		m_dispenserRetryTimer.Start( dispenserRebuildInterval );

		return SuspendFor( new CTFBotEngineerBuildDispenser, "Building a Dispenser" );
	}

	// try to build a Teleporter Exit
	const float exitRebuildInterval = 30.0f;
	if ( myTeleportExit )
	{
		// don't rebuild immediately after building is destroyed
		m_teleportExitRetryTimer.Start( exitRebuildInterval );
	}
	else if ( m_teleportExitRetryTimer.IsElapsed() && myTeleportEntrance && !isUnderAttack )
	{
		m_teleportExitRetryTimer.Start( exitRebuildInterval );

		// we need to build a teleporter exit yet
		if ( m_sentryBuildHint != NULL )
		{
			// if there are teleporter exit hints, find the closest one to our sentry and use it
			CUtlVector< CBaseEntity * > hintVector;
			CTFBotHintTeleporterExit *hint = NULL;
			while( ( hint = (CTFBotHintTeleporterExit *)gEntList.FindEntityByClassname( hint, "bot_hint_teleporter_exit" ) ) != NULL )
			{
				if ( hint->IsEnabled() && hint->InSameTeam( me ) )
				{
					hintVector.AddToTail( hint );
				}
			}

			if ( hintVector.Count() > 0 )
			{
				mySentry->UpdateLastKnownArea();
				CBaseEntity *closeHint = SelectClosestEntityByTravelDistance( me, hintVector, mySentry->GetLastKnownArea(), tf_bot_engineer_exit_near_sentry_range.GetFloat() );

				if ( closeHint )
				{
					return SuspendFor( new CTFBotEngineerBuildTeleportExit( closeHint->GetAbsOrigin(), closeHint->GetAbsAngles().y ), "Building teleporter exit at nearby hint" );
				}
			}
		}
		else if ( me->IsRangeLessThan( mySentry, 300.0f ) )
		{
			// drop a teleporter exit near our sentry
			return SuspendFor( new CTFBotEngineerBuildTeleportExit(), "Building teleporter exit" );
		}
	}

	// everything is built - maybe briefly assist another Engineer nest.
	// Keep this decision here, not inside UpgradeAndMaintainBuildings(), so the assist action owns its own PathFollower.
	if ( ShouldConsiderFriendlyEngineerActions( me ) )
	{
		UpdateFriendlySentryMemory( me );

		if ( m_assistFriendlyBuildingTimer.IsElapsed() )
		{
			m_assistFriendlyBuildingTimer.Start( RandomFloat( 3.0f, 6.0f ) );

			CBaseObject *assistTarget = FindFriendlyBuildingAssistTarget( me );
			if ( assistTarget )
			{
				return SuspendFor( new CTFBotEngineerAssistFriendlyBuilding( assistTarget ), "Helping a friendly Engineer building" );
			}
		}

		if ( m_patrolNestTimer.IsElapsed() )
		{
			m_patrolNestTimer.Start( RandomFloat( 10.0f, 20.0f ) );

			CBaseObject *patrolTarget = FindFriendlyNestPatrolTarget( me );
			if ( patrolTarget )
			{
				m_noFriendlyPatrolTargetTimer.Start( 30.0f );

				// Not every idle window should become a patrol. This keeps Engineers from abandoning their own nest too often.
				if ( RandomInt( 1, 100 ) <= 25 )
				{
					return SuspendFor( new CTFBotEngineerPatrolNest( patrolTarget ), "Patrolling a friendly Engineer nest" );
				}
			}
			else if ( m_noFriendlyPatrolTargetTimer.IsElapsed() )
			{
				m_noFriendlyPatrolTargetTimer.Start( 30.0f );
				return SuspendFor( new CTFBotEngineerPatrolNest, "Searching for friendly Engineer buildings" );
			}
		}

		if ( m_noFriendlyPatrolTargetTimer.IsElapsed() )
		{
			m_noFriendlyPatrolTargetTimer.Start( 30.0f );
			return SuspendFor( new CTFBotEngineerPatrolNest, "Searching for friendly Engineer buildings" );
		}
	}

	// everything is built - maintain them
	UpgradeAndMaintainBuildings( me );

	return Continue();
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::OnEnd( CTFBot *me, Action< CTFBot > *nextAction )
{
	me->StartLookingAroundForEnemies();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerBuilding::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerBuilding::OnTerritoryLost( CTFBot *me, int territoryID )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerBuilding::OnTerritoryCaptured( CTFBot *me, int territoryID )
{
	return TryContinue();
}
