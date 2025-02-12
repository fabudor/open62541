/* This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information.
 *
 *    Copyright 2016-2017 (c) Fraunhofer IOSB (Author: Julius Pfrommer)
 *    Copyright 2017 (c) Stefan Profanter, fortiss GmbH
 *    Copyright 2019 (c) HMS Industrial Networks AB (Author: Jonas Green)
 */

#include <open62541/plugin/accesscontrol_default.h>

/* Example access control management. Anonymous and username / password login.
 * The access rights are maximally permissive.
 *
 * FOR PRODUCTION USE, THIS EXAMPLE PLUGIN SHOULD BE REPLACED WITH LESS
 * PERMISSIVE ACCESS CONTROL.
 *
 * For TransferSubscriptions, we check whether the transfer happens between
 * Sessions for the same user. */

typedef struct {
    UA_Boolean allowAnonymous;
    size_t usernamePasswordLoginSize;
    UA_UsernamePasswordLogin *usernamePasswordLogin;
    UA_CertificateVerification verifyX509;
} AccessControlContext;

#define ANONYMOUS_POLICY "open62541-anonymous-policy"
#define CERTIFICATE_POLICY "open62541-certificate-policy"
#define USERNAME_POLICY "open62541-username-policy"
const UA_String anonymous_policy = UA_STRING_STATIC(ANONYMOUS_POLICY);
const UA_String certificate_policy = UA_STRING_STATIC(CERTIFICATE_POLICY);
const UA_String username_policy = UA_STRING_STATIC(USERNAME_POLICY);

/************************/
/* Access Control Logic */
/************************/

static UA_StatusCode
activateSession_default(UA_Server *server, UA_AccessControl *ac,
                        const UA_EndpointDescription *endpointDescription,
                        const UA_ByteString *secureChannelRemoteCertificate,
                        const UA_NodeId *sessionId,
                        const UA_ExtensionObject *userIdentityToken,
                        void **sessionContext) {
    AccessControlContext *context = (AccessControlContext*)ac->context;

    /* The empty token is interpreted as anonymous */
    UA_AnonymousIdentityToken anonToken;
    UA_ExtensionObject tmpIdentity;
    if(userIdentityToken->encoding == UA_EXTENSIONOBJECT_ENCODED_NOBODY) {
        UA_AnonymousIdentityToken_init(&anonToken);
        UA_ExtensionObject_init(&tmpIdentity);
        UA_ExtensionObject_setValueNoDelete(&tmpIdentity,
                                            &anonToken,
                                            &UA_TYPES[UA_TYPES_ANONYMOUSIDENTITYTOKEN]);
        userIdentityToken = &tmpIdentity;
    }

    /* Could the token be decoded? */
    if(userIdentityToken->encoding < UA_EXTENSIONOBJECT_DECODED)
        return UA_STATUSCODE_BADIDENTITYTOKENINVALID;

    /* If the session has been activated before, the same (decoded)
     * UserIdentityToken must be used. */
    if(*sessionContext) {
        UA_ExtensionObject *oldUserIdentityToken = *(UA_ExtensionObject**)sessionContext;
        if(UA_order(userIdentityToken, oldUserIdentityToken,
                    &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]) != UA_ORDER_EQ)
            return UA_STATUSCODE_BADUSERACCESSDENIED;
        return UA_STATUSCODE_GOOD;
    }

    const UA_DataType *tokenType = userIdentityToken->content.decoded.type;
    if(tokenType == &UA_TYPES[UA_TYPES_ANONYMOUSIDENTITYTOKEN]) {
        /* Anonymous login */
        if(!context->allowAnonymous)
            return UA_STATUSCODE_BADIDENTITYTOKENINVALID;

        const UA_AnonymousIdentityToken *token = (UA_AnonymousIdentityToken*)
            userIdentityToken->content.decoded.data;

        /* Compatibility notice: Siemens OPC Scout v10 provides an empty
         * policyId. This is not compliant. For compatibility, assume that empty
         * policyId == ANONYMOUS_POLICY */
        if(token->policyId.data && !UA_String_equal(&token->policyId, &anonymous_policy))
            return UA_STATUSCODE_BADIDENTITYTOKENINVALID;
    } else if(tokenType == &UA_TYPES[UA_TYPES_USERNAMEIDENTITYTOKEN]) {
        /* Username and password */
        const UA_UserNameIdentityToken *userToken =
            (UA_UserNameIdentityToken*)userIdentityToken->content.decoded.data;
        if(!UA_String_equal(&userToken->policyId, &username_policy))
            return UA_STATUSCODE_BADIDENTITYTOKENINVALID;

        /* The userToken has been decrypted by the server before forwarding
         * it to the plugin. This information can be used here. */
        /* if(userToken->encryptionAlgorithm.length > 0) {} */

        /* Empty username and password */
        if(userToken->userName.length == 0 && userToken->password.length == 0)
            return UA_STATUSCODE_BADIDENTITYTOKENINVALID;

        /* Try to match username/pw */
        UA_Boolean match = false;
        for(size_t i = 0; i < context->usernamePasswordLoginSize; i++) {
            if(UA_String_equal(&userToken->userName,
                               &context->usernamePasswordLogin[i].username) &&
               UA_String_equal(&userToken->password,
                               &context->usernamePasswordLogin[i].password)) {
                match = true;
                break;
            }
        }
        if(!match)
            return UA_STATUSCODE_BADUSERACCESSDENIED;
    } else if(tokenType == &UA_TYPES[UA_TYPES_X509IDENTITYTOKEN]) {
        /* x509 certificate */
        const UA_X509IdentityToken *userToken = (UA_X509IdentityToken*)
            userIdentityToken->content.decoded.data;

        if(!UA_String_equal(&userToken->policyId, &certificate_policy))
            return UA_STATUSCODE_BADIDENTITYTOKENINVALID;

        if(!context->verifyX509.verifyCertificate)
            return UA_STATUSCODE_BADIDENTITYTOKENINVALID;

        UA_StatusCode res = context->verifyX509.
            verifyCertificate(&context->verifyX509,
                              &userToken->certificateData);
        if(res != UA_STATUSCODE_GOOD)
            return res;
    } else {
        /* Unsupported token type */
        return UA_STATUSCODE_BADIDENTITYTOKENINVALID;
    }

    /* Login successful. Store the token in the context. */
    UA_ExtensionObject *tokenCopy = UA_ExtensionObject_new();
    if(!tokenCopy)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    UA_StatusCode res = UA_ExtensionObject_copy(userIdentityToken, tokenCopy);
    if(res != UA_STATUSCODE_GOOD) {
        UA_ExtensionObject_delete(tokenCopy);
        return UA_STATUSCODE_BADOUTOFMEMORY;
    }
    *sessionContext = tokenCopy;

    return UA_STATUSCODE_GOOD;
}

static void
closeSession_default(UA_Server *server, UA_AccessControl *ac,
                     const UA_NodeId *sessionId, void *sessionContext) {
    if(sessionContext)
        UA_ExtensionObject_delete((UA_ExtensionObject*)sessionContext);
}

static UA_UInt32
getUserRightsMask_default(UA_Server *server, UA_AccessControl *ac,
                          const UA_NodeId *sessionId, void *sessionContext,
                          const UA_NodeId *nodeId, void *nodeContext) {
    return 0xFFFFFFFF;
}

static UA_Byte
getUserAccessLevel_default(UA_Server *server, UA_AccessControl *ac,
                           const UA_NodeId *sessionId, void *sessionContext,
                           const UA_NodeId *nodeId, void *nodeContext) {
    return 0xFF;
}

static UA_Boolean
getUserExecutable_default(UA_Server *server, UA_AccessControl *ac,
                          const UA_NodeId *sessionId, void *sessionContext,
                          const UA_NodeId *methodId, void *methodContext) {
    return true;
}

static UA_Boolean
getUserExecutableOnObject_default(UA_Server *server, UA_AccessControl *ac,
                                  const UA_NodeId *sessionId, void *sessionContext,
                                  const UA_NodeId *methodId, void *methodContext,
                                  const UA_NodeId *objectId, void *objectContext) {
    return true;
}

static UA_Boolean
allowAddNode_default(UA_Server *server, UA_AccessControl *ac,
                     const UA_NodeId *sessionId, void *sessionContext,
                     const UA_AddNodesItem *item) {
    return true;
}

static UA_Boolean
allowAddReference_default(UA_Server *server, UA_AccessControl *ac,
                          const UA_NodeId *sessionId, void *sessionContext,
                          const UA_AddReferencesItem *item) {
    return true;
}

static UA_Boolean
allowDeleteNode_default(UA_Server *server, UA_AccessControl *ac,
                        const UA_NodeId *sessionId, void *sessionContext,
                        const UA_DeleteNodesItem *item) {
    return true;
}

static UA_Boolean
allowDeleteReference_default(UA_Server *server, UA_AccessControl *ac,
                             const UA_NodeId *sessionId, void *sessionContext,
                             const UA_DeleteReferencesItem *item) {
    return true;
}

static UA_Boolean
allowBrowseNode_default(UA_Server *server, UA_AccessControl *ac,
                        const UA_NodeId *sessionId, void *sessionContext,
                        const UA_NodeId *nodeId, void *nodeContext) {
    return true;
}

#ifdef UA_ENABLE_SUBSCRIPTIONS
static UA_Boolean
allowTransferSubscription_default(UA_Server *server, UA_AccessControl *ac,
                                  const UA_NodeId *oldSessionId, void *oldSessionContext,
                                  const UA_NodeId *newSessionId, void *newSessionContext) {
    if(oldSessionContext == newSessionContext)
        return true;
    if(oldSessionContext && newSessionContext)
        return (UA_order(oldSessionContext, newSessionContext,
                         &UA_TYPES[UA_TYPES_EXTENSIONOBJECT]) == UA_ORDER_EQ);
    return false;
}
#endif

#ifdef UA_ENABLE_HISTORIZING
static UA_Boolean
allowHistoryUpdateUpdateData_default(UA_Server *server, UA_AccessControl *ac,
                                     const UA_NodeId *sessionId, void *sessionContext,
                                     const UA_NodeId *nodeId,
                                     UA_PerformUpdateType performInsertReplace,
                                     const UA_DataValue *value) {
    return true;
}

static UA_Boolean
allowHistoryUpdateDeleteRawModified_default(UA_Server *server, UA_AccessControl *ac,
                                            const UA_NodeId *sessionId, void *sessionContext,
                                            const UA_NodeId *nodeId,
                                            UA_DateTime startTimestamp,
                                            UA_DateTime endTimestamp,
                                            bool isDeleteModified) {
    return true;
}
#endif

/***************************************/
/* Create Delete Access Control Plugin */
/***************************************/

static void clear_default(UA_AccessControl *ac) {
    UA_Array_delete((void*)(uintptr_t)ac->userTokenPolicies,
                    ac->userTokenPoliciesSize,
                    &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
    ac->userTokenPolicies = NULL;
    ac->userTokenPoliciesSize = 0;

    AccessControlContext *context = (AccessControlContext*)ac->context;

    if (context) {
        for(size_t i = 0; i < context->usernamePasswordLoginSize; i++) {
            UA_String_clear(&context->usernamePasswordLogin[i].username);
            UA_String_clear(&context->usernamePasswordLogin[i].password);
        }
        if(context->usernamePasswordLoginSize > 0)
            UA_free(context->usernamePasswordLogin);

        if(context->verifyX509.clear)
            context->verifyX509.clear(&context->verifyX509);

        UA_free(ac->context);
        ac->context = NULL;
    }
}

UA_StatusCode
UA_AccessControl_default(UA_ServerConfig *config,
                         UA_Boolean allowAnonymous,
                         UA_CertificateVerification *verifyX509,
                         const UA_ByteString *userTokenPolicyUri,
                         size_t usernamePasswordLoginSize,
                         const UA_UsernamePasswordLogin *usernamePasswordLogin) {
    UA_LOG_WARNING(&config->logger, UA_LOGCATEGORY_SERVER,
                   "AccessControl: Unconfigured AccessControl. Users have all permissions.");
    UA_AccessControl *ac = &config->accessControl;

    if(ac->clear)
        clear_default(ac);

    ac->clear = clear_default;
    ac->activateSession = activateSession_default;
    ac->closeSession = closeSession_default;
    ac->getUserRightsMask = getUserRightsMask_default;
    ac->getUserAccessLevel = getUserAccessLevel_default;
    ac->getUserExecutable = getUserExecutable_default;
    ac->getUserExecutableOnObject = getUserExecutableOnObject_default;
    ac->allowAddNode = allowAddNode_default;
    ac->allowAddReference = allowAddReference_default;
    ac->allowBrowseNode = allowBrowseNode_default;

#ifdef UA_ENABLE_SUBSCRIPTIONS
    ac->allowTransferSubscription = allowTransferSubscription_default;
#endif

#ifdef UA_ENABLE_HISTORIZING
    ac->allowHistoryUpdateUpdateData = allowHistoryUpdateUpdateData_default;
    ac->allowHistoryUpdateDeleteRawModified = allowHistoryUpdateDeleteRawModified_default;
#endif

    ac->allowDeleteNode = allowDeleteNode_default;
    ac->allowDeleteReference = allowDeleteReference_default;

    AccessControlContext *context = (AccessControlContext*)
            UA_malloc(sizeof(AccessControlContext));
    if(!context)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    memset(context, 0, sizeof(AccessControlContext));
    ac->context = context;

    /* Allow anonymous? */
    context->allowAnonymous = allowAnonymous;
    if(allowAnonymous) {
        UA_LOG_INFO(&config->logger, UA_LOGCATEGORY_SERVER,
                    "AccessControl: Anonymous login is enabled");
    }

    if(config->logging == NULL) {
        config->logging = &config->logger;
    }
    /* Allow x509 certificates? Move the plugin over. */
    if(verifyX509) {
        context->verifyX509 = *verifyX509;
        memset(verifyX509, 0, sizeof(UA_CertificateVerification));
        if(context->verifyX509.logging == NULL) {
            context->verifyX509.logging = &config->logging;
        }
    } else {
        memset(&context->verifyX509, 0, sizeof(UA_CertificateVerification));
        context->verifyX509.logging = &config->logging;
        UA_LOG_INFO(&config->logger, UA_LOGCATEGORY_SERVER,
                    "AccessControl: x509 certificate user authentication is enabled");
    }

    /* Copy username/password to the access control plugin */
    if(usernamePasswordLoginSize > 0) {
        context->usernamePasswordLogin = (UA_UsernamePasswordLogin*)
            UA_malloc(usernamePasswordLoginSize * sizeof(UA_UsernamePasswordLogin));
        if(!context->usernamePasswordLogin)
            return UA_STATUSCODE_BADOUTOFMEMORY;
        context->usernamePasswordLoginSize = usernamePasswordLoginSize;
        for(size_t i = 0; i < usernamePasswordLoginSize; i++) {
            UA_String_copy(&usernamePasswordLogin[i].username,
                           &context->usernamePasswordLogin[i].username);
            UA_String_copy(&usernamePasswordLogin[i].password,
                           &context->usernamePasswordLogin[i].password);
        }
    }

    /* Set the allowed policies */
    size_t policies = 0;
    if(allowAnonymous)
        policies++;
    if(verifyX509)
        policies++;
    if(usernamePasswordLoginSize > 0)
        policies++;
    ac->userTokenPoliciesSize = 0;
    ac->userTokenPolicies = (UA_UserTokenPolicy *)
        UA_Array_new(policies, &UA_TYPES[UA_TYPES_USERTOKENPOLICY]);
    if(!ac->userTokenPolicies)
        return UA_STATUSCODE_BADOUTOFMEMORY;
    ac->userTokenPoliciesSize = policies;

    policies = 0;
    if(allowAnonymous) {
        ac->userTokenPolicies[policies].tokenType = UA_USERTOKENTYPE_ANONYMOUS;
        ac->userTokenPolicies[policies].policyId = UA_STRING_ALLOC(ANONYMOUS_POLICY);
        policies++;
    }

    if(verifyX509) {
        ac->userTokenPolicies[policies].tokenType = UA_USERTOKENTYPE_CERTIFICATE;
        ac->userTokenPolicies[policies].policyId = UA_STRING_ALLOC(CERTIFICATE_POLICY);
#if UA_LOGLEVEL <= 400
        if(UA_ByteString_equal(userTokenPolicyUri, &UA_SECURITY_POLICY_NONE_URI)) {
            UA_LOG_WARNING(&config->logger, UA_LOGCATEGORY_SERVER,
                           "x509 Certificate Authentication configured, "
                           "but no encrypting SecurityPolicy. "
                           "This can leak credentials on the network.");
        }
#endif
        UA_ByteString_copy(userTokenPolicyUri,
                           &ac->userTokenPolicies[policies].securityPolicyUri);
        policies++;
    }

    if(usernamePasswordLoginSize > 0) {
        ac->userTokenPolicies[policies].tokenType = UA_USERTOKENTYPE_USERNAME;
        ac->userTokenPolicies[policies].policyId = UA_STRING_ALLOC(USERNAME_POLICY);
#if UA_LOGLEVEL <= 400
        if(UA_ByteString_equal(userTokenPolicyUri, &UA_SECURITY_POLICY_NONE_URI)) {
            UA_LOG_WARNING(&config->logger, UA_LOGCATEGORY_SERVER,
                           "Username/Password Authentication configured, "
                           "but no encrypting SecurityPolicy. "
                           "This can leak credentials on the network.");
        }
#endif
        UA_ByteString_copy(userTokenPolicyUri,
                           &ac->userTokenPolicies[policies].securityPolicyUri);
    }
    return UA_STATUSCODE_GOOD;
}

