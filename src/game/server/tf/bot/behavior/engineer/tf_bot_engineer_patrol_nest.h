//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_patrol_nest.h
// Engineer patrolling between his nest and a known friendly Engineer building

#ifndef TF_BOT_ENGINEER_PATROL_NEST_H
#define TF_BOT_ENGINEER_PATROL_NEST_H

#include "Path/NextBotPathFollow.h"

class CBaseObject;
class CObjectSentrygun;
class CTFPlayer;

class CTFBotEngineerPatrolNest : public Action< CTFBot >
{
public:
	CTFBotEngineerPatrolNest( CBaseObject *targetBuilding );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );
	virtual void					OnEnd( CTFBot *me, Action< CTFBot > *nextAction );
	virtual ActionResult< CTFBot >	OnResume( CTFBot *me, Action< CTFBot > *interruptingAction );
	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );

	virtual QueryResultType	ShouldHurry( const INextBot *me ) const;

	virtual const char *GetName( void ) const	{ return "EngineerPatrolNest"; };

private:
	enum SpyCheckState
	{
		SPYCHECK_NONE = 0,
		SPYCHECK_AIM,
		SPYCHECK_FIRE,
		SPYCHECK_WAIT_BETWEEN_SHOTS,
		SPYCHECK_DONE
	};

	CHandle< CBaseObject > m_targetBuilding;
	CHandle< CObjectSentrygun > m_homeSentry;

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_waitTimer;
	CountdownTimer m_giveUpTimer;
	CountdownTimer m_spyCheckTimer;

	bool m_hasArrived;
	bool m_shouldSpyCheck;
	int m_spyCheckShotsRemaining;
	SpyCheckState m_spyCheckState;

	bool IsTargetValid( CTFBot *me, CBaseObject *target ) const;
	bool MyImportantBuildingsNeedMe( CTFBot *me ) const;
	bool IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const;
	bool IsVisibleBuildingForEngineer( CTFBot *me, CBaseObject *obj ) const;
	CTFPlayer *FindFriendlyNonEngineerStandingOnBuilding( CTFBot *me, CBaseObject *target ) const;
	Vector GetSpyCheckAimSpot( CTFBot *me, CBaseObject *target ) const;
	bool UpdateSpyCheck( CTFBot *me, CBaseObject *target );
};

#endif // TF_BOT_ENGINEER_PATROL_NEST_H
