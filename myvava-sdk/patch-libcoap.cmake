# Keep the upstream archive pinned. Block1 retransmissions use a shorter state
# token; translating it for a disconnect NACK can resize the PDU and invalidate
# the original token pointer before libcoap restores it (ASan: coap_session.c).
set(coap_session_path "${myvava_coap_SOURCE_DIR}/src/coap_session.c")
file(READ "${coap_session_path}" coap_session_source)
set(coap_session_before [=[        coap_bin_const_t token = q->pdu->actual_token;

        coap_check_update_token(session, q->pdu);
        coap_lock_callback(session->context,
                           session->context->nack_handler(session, q->pdu, reason, q->id));
        coap_update_token(q->pdu, token.length, token.s);
]=])
set(coap_session_after [=[        /* myvava: preserve the token across PDU resize during NACK. */
        coap_bin_const_t *token = coap_new_bin_const(q->pdu->actual_token.s,
                                                    q->pdu->actual_token.length);
        if (token)
          coap_check_update_token(session, q->pdu);
        coap_lock_callback(session->context,
                           session->context->nack_handler(session, q->pdu, reason, q->id));
        if (token) {
          coap_update_token(q->pdu, token->length, token->s);
          coap_delete_bin_const(token);
        }
]=])
string(FIND "${coap_session_source}" "myvava: preserve the token across PDU resize" coap_session_patched)
if(coap_session_patched EQUAL -1)
  string(FIND "${coap_session_source}" "${coap_session_before}" coap_session_match)
  if(coap_session_match EQUAL -1)
    message(FATAL_ERROR "Pinned libcoap disconnect-token patch no longer matches")
  endif()
  string(REPLACE "${coap_session_before}" "${coap_session_after}" coap_session_source "${coap_session_source}")
  file(WRITE "${coap_session_path}" "${coap_session_source}")
endif()
