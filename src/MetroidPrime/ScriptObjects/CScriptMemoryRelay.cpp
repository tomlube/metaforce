#include "MetroidPrime/ScriptObjects/CScriptMemoryRelay.hpp"

#include "MetroidPrime/CScriptMailbox.hpp"

#if defined(TARGET_PC)
#include "Metaforce/MergedWorld.hpp"
#endif

CScriptMemoryRelay::CScriptMemoryRelay(TUniqueId uid, const rstl::string& name,
                                       const CEntityInfo& info, bool defaultActive,
                                       bool skipSendActive, bool ignoreMessages)
: CEntity(uid, info, true, name)
, mDefaultActive(defaultActive)
, mSkipSendActive(skipSendActive)
, mIgnoreMessages(ignoreMessages) {}

CScriptMemoryRelay::~CScriptMemoryRelay() {}

void CScriptMemoryRelay::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId objId, CStateManager& stateMgr) {
  if (mIgnoreMessages) {
    return;
  }

  switch (msg) {
    case kSM_Activate:
#if defined(TARGET_PC)
    {
      // A relay of an area appended from another region keeps its state in that region.
      uint relayId = GetEditorId().Value();
      metaforce::merged::GetMailboxForEditorId(stateMgr, relayId)->AddMsg(TEditorId(relayId));
    }
#else
      stateMgr.Mailbox()->AddMsg(GetEditorId());
#endif
      if (!mSkipSendActive) {
        SendScriptMsgs(kSS_Active, stateMgr, kSM_None);
      }
      break;

    case kSM_Deactivate:
#if defined(TARGET_PC)
    {
      uint relayId = GetEditorId().Value();
      metaforce::merged::GetMailboxForEditorId(stateMgr, relayId)->RemoveMsg(TEditorId(relayId));
    }
#else
      stateMgr.Mailbox()->RemoveMsg(GetEditorId());
#endif
      break;
    
    default:
      CEntity::AcceptScriptMsg(msg, objId, stateMgr);
      break;
  }
}

ENTITY_ACCEPT_IMPL(CScriptMemoryRelay)
