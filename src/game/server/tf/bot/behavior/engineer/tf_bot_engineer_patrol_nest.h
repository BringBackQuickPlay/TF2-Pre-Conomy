//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_patrol_nest.h
// Engineer patrolling/searching between his nest and nearby friendly Engineer buildings

#ifndef TF_BOT_ENGINEER_PATROL_NEST_H
#define TF_BOT_ENGINEER_PATROL_NEST_H

#include "Path/NextBotPathFollow.h"

class CBaseObject;
class CObjectSentrygun;
class CTFPlayer;

class CTFBotEngineerPatrolNest : public Action< CTFBot >
{
public:
	CTFBotEngineerPatrolNest( void );
	CTFBotEngineerPatrolNest( CBaseObject *targetBuilding );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );
	virtual void					OnEnd( CTFBot *me, Action< CTFBot > *nextAction );
	virtual ActionResult< CTFBot >	OnResume( CTFBot *me, Action< CTFBot > *interruptingAction );
	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );

	virtual QueryResultType	ShouldHurry( const INextBot *me ) const;

	virtual const char *GetName( void ) const	{ return "EngineerPatrolNest"; };

private:
	enum PatrolMode
	{
		PATROL_MODE_TARGET = 0,
		PATROL_MODE_SEARCH_FOR_FRIENDLY_BUILDINGS
	};

	enum SearchPhase
	{
		SEARCH_PHASE_SIDESTEP = 0,
		SEARCH_PHASE_WANDER
	};

	PatrolMode m_mode;
	SearchPhase m_searchPhase;

	CHandle< CBaseObject > m_targetBuilding;

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_waitTimer;
	CountdownTimer m_spyCheckShotTimer;
	CountdownTimer m_giveUpTimer;

	Vector m_searchGoal;
	bool m_hasSearchGoal;
	bool m_hasArrived;
	bool m_shouldSpyCheck;
	int m_spyCheckShotsRemaining;

	bool IsTargetValid( CTFBot *me, CBaseObject *target ) const;
	bool MyImportantBuildingsNeedMe( CTFBot *me ) const;
	bool IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const;
	bool IsVisibleBuildingForEngineer( CTFBot *me, CBaseObject *obj ) const;
	CBaseObject *FindVisibleFriendlyNestTarget( CTFBot *me ) const;
	bool SelectSearchGoal( CTFBot *me );
	bool UpdateSearchForFriendlyBuildings( CTFBot *me );
	CTFPlayer *FindFriendlyNonEngineerStandingOnBuilding( CTFBot *me, CBaseObject *target ) const;
	Vector GetSpyCheckAimSpot( CTFBot *me, CBaseObject *target ) const;
	bool SpyCheckBuilding( CTFBot *me, CBaseObject *target );
};

#endif // TF_BOT_ENGINEER_PATROL_NEST_H
