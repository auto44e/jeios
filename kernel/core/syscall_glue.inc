/*
 * Shared frame-neutral endpoint/handle syscall glue.
 *
 * The including translation unit supplies:
 *   JE_G_ARG(i), JE_G_SET_STATUS(v), JE_G_SET_RESULT(v), JE_G_RETURN(),
 *   JE_G_BLOCK(rec, side, deadline), JE_G_FINISH_FN and
 *   JE_G_FINISH_PARAMS.
 *
 * x86 expands these accessors to the existing trap-frame fields and return
 * seams.  AArch64 expands them to x0..x5, out_status/out_result, and the ARM
 * park action.  The architecture-specific PROCESS/DEVICE and test-policy
 * seams are supplied by JE_G_* macros in the including wrapper.
 */

#ifdef JE_G_EMIT_FINISH
JE_G_FINISH_STORAGE void JE_G_FINISH_FN(JE_G_FINISH_PARAMS)
{
    je_vm *vm; uint32_t index;
    if (!current_vm_index(&vm, &index)) {
        JE_G_SET_STATUS(JE_ABI_INVALID); JE_G_SET_RESULT(0); return;
    }
    je_ep_wait_cause cause = je_endpoint_wait_cause(rec, side);
    uint32_t slot = 0u; (void)je_sched_current_slot(&slot);
    switch (cause) {
    case JE_EP_WAIT_TIMED_OUT:
        je_endpoint_kern_wait_release(rec, side, slot);
        JE_G_NOTE_WAIT_TERMINAL(index, rec, side, JE_EP_WAIT_TIMED_OUT);
        JE_G_SET_STATUS(JE_ABI_H_TIMED_OUT); JE_G_SET_RESULT(0); return;
    case JE_EP_WAIT_PEER_CLOSED:
        je_endpoint_kern_wait_release(rec, side, slot);
        JE_G_NOTE_WAIT_TERMINAL(index, rec, side, JE_EP_WAIT_PEER_CLOSED);
        JE_G_SET_STATUS(JE_ABI_H_PEER_CLOSED); JE_G_SET_RESULT(0); return;
    case JE_EP_WAIT_CANCELLED:
        je_endpoint_kern_wait_release(rec, side, slot);
        JE_G_SET_STATUS(JE_ABI_H_CANCELLED); JE_G_SET_RESULT(0); return;
    case JE_EP_WAIT_WOKEN: break;
    default:
        JE_G_IPC_FATAL("recv resume without terminal cause");
        return;
    }

    uint8_t staging[JE_EP_MSG_MAX];
    je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
    size_t mlen = 0u; uint8_t mcaps = 0u;
    je_ep_recv_lease lease;
    je_ep_status s = je_endpoint_kern_recv_peek_owned(rec, side, staging,
                                                      je_syscall_recv_staging_cap(cap, sizeof staging),
                                                      &mlen, (uint8_t)hcap, caps, &mcaps,
                                                      &lease);
    if (s == JE_EP_SHORT_BUFFER) {
        je_endpoint_kern_wait_release(rec, side, slot);
        JE_G_NOTE_SHORT_BUFFER(index, rec, side, mlen, mcaps);
        JE_G_SET_STATUS(JE_ABI_H_SHORT_BUFFER);
        JE_G_SET_RESULT((uint64_t)mlen | ((uint64_t)mcaps << 32)); return;
    }
    if (s != JE_EP_OK) { je_endpoint_kern_wait_release(rec, side, slot);
                         JE_G_SET_STATUS(ep_status_to_abi(s));
                         JE_G_SET_RESULT(0); return; }
    je_handle minted[JE_ABI_MSG_CAPS_MAX]; bool was_live[JE_ABI_MSG_CAPS_MAX];
    for (uint8_t c = 0; c < mcaps; ++c) { minted[c]=0u; was_live[c]=false; }
    je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
    for (uint8_t c = 0; c < mcaps; ++c) {
        if (!je_endpoint_kern_capdesc_live(&caps[c])) { minted[c]=0u; continue; }
        je_handle nh = 0u;
        je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
        if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
        minted[c]=nh; was_live[c]=true; done=(uint8_t)(c+1u);
    }
    if (fail != JE_ABI_OK) {
        for (uint8_t c = 0u; c < done; ++c)
            if (was_live[c] && minted[c])
                (void)je_endpoint_kern_close(vm, index, minted[c]);
        je_endpoint_kern_wait_release(rec, side, slot);
        JE_G_SET_STATUS(fail); JE_G_SET_RESULT(0); return;
    }
    if (mlen != 0u) {
        je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
        if (cs != JE_ABI_OK) {
            for (uint8_t c = 0; c < mcaps; ++c)
                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
            je_endpoint_kern_wait_release(rec, side, slot);
            JE_G_FAULT_RESUMED_COPY();
            JE_G_SET_STATUS(JE_ABI_INVALID); JE_G_SET_RESULT(0); return;
        }
    }
    if (mcaps != 0u) {
        uint32_t hout[JE_ABI_MSG_CAPS_MAX];
        for (uint8_t c = 0; c < mcaps; ++c) hout[c]=(uint32_t)minted[c];
        je_abi_status cs = je_copy_to_user(hbuf, hout, (uint64_t)mcaps*sizeof(uint32_t));
        if (cs != JE_ABI_OK) {
            for (uint8_t c = 0; c < mcaps; ++c)
                if (was_live[c] && minted[c]) (void)je_endpoint_kern_close(vm, index, minted[c]);
            je_endpoint_kern_wait_release(rec, side, slot);
            JE_G_SET_STATUS(JE_ABI_INVALID); JE_G_SET_RESULT(0); return;
        }
    }
    JE_G_FAULT_RECV_COMMIT(lease);
    je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
    if (cm != JE_EP_OK) {
        JE_G_IPC_FATAL("blocked recv commit mismatch");
    }
    JE_G_NOTE_RECV_CAPS(index, rec, side, mcaps, caps, minted, was_live);
#if JE_TEST_MODE == 13
    for (uint8_t c = 0u; c < mcaps; ++c)
        if (was_live[c] && minted[c])
            je_m4g_record_cap_delivery(vm, index, &caps[c]);
    je_m4g_record_control_receive(vm, index, &lease, staging, mlen, mcaps);
#endif
#if JE_TEST_MODE == 15
    for (uint8_t c = 0u; c < mcaps; ++c)
        if (was_live[c] && minted[c])
            je_m4g_record_cap_delivery(vm, index, &caps[c]);
#endif
    je_endpoint_kern_wait_release(rec, side, slot);
    JE_G_SET_STATUS(JE_ABI_OK);
    JE_G_SET_RESULT((uint64_t)mlen | ((uint64_t)mcaps << 32));
}
#endif

#ifdef JE_G_EMIT_CASES_EARLY
    case JE_SYSCALL_HANDLE_CLOSE: {
        /* Typed close dispatch (A3-3): peek the slot's object type, then branch
         * EXACTLY — SELF -> raw table close; ENDPOINT -> endpoint-aware close
         * (drops the handle_ref); any other/unknown type -> fail closed. */
        je_vm *vm;
        uint32_t index;
        if (!current_vm_index(&vm, &index)) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        je_handle_table *t = &vm->procs[index].htable;
        je_handle h = (je_handle)JE_G_ARG(0);
        uint32_t type = 0u;
        uint32_t rights = 0u;
        void *object = (void *)0;
        je_handle_status resolve = je_handle_resolve_any(
            t, h, &type, &rights, &object);
        if (resolve != JE_HANDLE_OK) {
            JE_G_SET_STATUS(handle_status_to_abi(resolve));
            JE_G_SET_RESULT(0);
        } else if (type == JE_ABI_OBJ_ENDPOINT) {
            je_ep_status s = je_endpoint_kern_close(vm, index, h);
            JE_G_SET_STATUS(ep_status_to_abi(s));
            JE_G_SET_RESULT(0);
        JE_G_CLOSE_EXTRA(t, h, type, rights, object)
        } else {
            JE_G_CLOSE_FALLBACK();
            JE_G_SET_RESULT(0);
        }
        JE_G_RETURN();
    }
    case JE_SYSCALL_HANDLE_QUERY: {
        je_handle_table *t = current_htable();
        if (!t) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        uint32_t type = 0u;
        uint32_t rights = 0u;
        void *object = (void *)0;
        je_handle_status s = je_handle_resolve_any(
            t, (je_handle)JE_G_ARG(0), &type, &rights, &object);
        JE_G_QUERY_RESULT(s, type, rights, object);
        JE_G_RETURN();
    }
    case JE_SYSCALL_HANDLE_RIGHTS: {
        je_handle_table *t = current_htable();
        if (!t) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        uint32_t type = 0u;
        uint32_t rights = 0u;
        void *object = (void *)0;
        je_handle_status s = je_handle_resolve_any(
            t, (je_handle)JE_G_ARG(0), &type, &rights, &object);
        JE_G_RIGHTS_RESULT(s, type, rights, object);
        JE_G_RETURN();
    }
    case JE_SYSCALL_HANDLE_DUP: {
        je_handle_table *t = current_htable();
        if (!t) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        je_handle nh = 0u;
        uint32_t type = 0u;
        uint32_t source_rights = 0u;
        void *object = (void *)0;
        je_handle_status resolve = je_handle_resolve_any(
            t, (je_handle)JE_G_ARG(0), &type, &source_rights, &object);
        JE_G_DUP_PREPARE(resolve, type)
        if (resolve != JE_HANDLE_OK) {
            JE_G_SET_STATUS(handle_status_to_abi(resolve));
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        uint32_t new_rights = (uint32_t)JE_G_ARG(1);
        JE_G_DUP_RESOLVED(t, type, source_rights, object, new_rights, nh);
        JE_G_SET_RESULT((uint64_t)nh);
        JE_G_RETURN();
    }
    case JE_SYSCALL_ENDPOINT_CREATE: {
        je_vm *vm;
        uint32_t index;
        if (!current_vm_index(&vm, &index)) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        je_handle h0 = 0u;
        je_handle h1 = 0u;
        je_ep_status s = je_endpoint_kern_create(vm, index, &h0, &h1);
        JE_G_SET_STATUS(ep_status_to_abi(s));
        JE_G_SET_RESULT(s == JE_EP_OK
               ? ((uint64_t)h0 | ((uint64_t)h1 << 32)) : 0u);
        JE_G_RETURN();
    }
#endif
#ifdef JE_G_EMIT_CASES_LATE
    case JE_SYSCALL_TRY_SEND: {
        je_vm *vm;
        uint32_t index;
        if (!current_vm_index(&vm, &index)) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        je_handle h = (je_handle)JE_G_ARG(0);
        uint64_t ubuf = JE_G_ARG(1), len = JE_G_ARG(2);
        uint8_t ncaps = (uint8_t)(JE_G_ARG(3) & 0xffu);
        uint64_t caparr = JE_G_ARG(4);
        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
        je_ep_status bpf = je_endpoint_kern_send_preflight(vm, index, h, (size_t)len);
        if (bpf != JE_EP_OK) {
            JE_G_SET_STATUS(ep_status_to_abi(bpf));
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        if (ncaps != 0u) {
            uint64_t raw[JE_ABI_MSG_CAPS_MAX];
            je_abi_status cs = je_copy_from_user(
                raw, caparr, (uint64_t)ncaps * sizeof(uint64_t));
            if (cs != JE_ABI_OK) {
                JE_G_SET_STATUS(JE_ABI_INVALID);
                JE_G_SET_RESULT(0);
                JE_G_RETURN();
            }
            for (uint8_t c = 0u; c < ncaps; ++c) {
                je_handle ch = (je_handle)(raw[c] & UINT64_C(0xffffffff));
                uint32_t crights = (uint32_t)(raw[c] >> 32);
                if (crights & ~(uint32_t)JE_ABI_RIGHTS_KNOWN) {
                    JE_G_SET_STATUS(JE_ABI_INVALID);
                    JE_G_SET_RESULT(0);
                    JE_G_RETURN();
                }
                je_ep_status rc = je_endpoint_kern_resolve_send_cap(
                    vm, index, ch, crights, &caps[c]);
                if (rc != JE_EP_OK) {
                    JE_G_SET_STATUS(ep_status_to_abi(rc));
                    JE_G_SET_RESULT(0);
                    JE_G_RETURN();
                }
            }
        }
        uint8_t staging[JE_EP_MSG_MAX];
        if (len != 0u) {
            je_abi_status cs = je_copy_from_user(staging, ubuf, len);
            if (cs != JE_ABI_OK) {
                JE_G_SET_STATUS(JE_ABI_INVALID);
                JE_G_SET_RESULT(0);
                JE_G_RETURN();
            }
        }
        je_ep_status s = je_endpoint_kern_send_caps(
            vm, index, h, staging, (size_t)len, caps, ncaps);
        JE_G_SET_STATUS(ep_status_to_abi(s));
        JE_G_SET_RESULT(0);
        JE_G_RETURN();
    }
    case JE_SYSCALL_TRY_RECV: {
        je_vm *vm;
        uint32_t index;
        if (!current_vm_index(&vm, &index)) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        je_handle h = (je_handle)JE_G_ARG(0);
        uint64_t ubuf = JE_G_ARG(1), cap = JE_G_ARG(2),
                 hbuf = JE_G_ARG(4), hcap = JE_G_ARG(5);
        uint64_t timeout = JE_G_ARG(3);
        uint64_t now = je_sched_ticks();
        if (timeout != 0u &&
            (timeout > JE_IPC_MAX_WAIT || timeout > (UINT64_MAX - now))) {
            JE_G_SET_STATUS(JE_ABI_INVALID);
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        if (timeout != 0u) {
            je_ep_status probe = je_endpoint_kern_recv_entry_probe(vm, index, h);
            if (probe == JE_EP_EMPTY) {
                uint32_t slot = 0u;
                if (je_sched_current_slot(&slot) != JE_OK) {
                    JE_G_SET_STATUS(JE_ABI_INVALID);
                    JE_G_SET_RESULT(0);
                    JE_G_RETURN();
                }
                je_ep_channel *wrec = (je_ep_channel *)0;
                uint8_t wside = 0u;
                je_ep_status reg = je_endpoint_kern_wait_register(
                    vm, index, h, slot, &wrec, &wside);
                if (reg == JE_EP_WAITERS_BUSY) {
                    JE_G_SET_STATUS(JE_ABI_H_EMPTY);
                    JE_G_SET_RESULT(0);
                    JE_G_RETURN();
                }
                if (reg != JE_EP_OK) {
                    JE_G_SET_STATUS(ep_status_to_abi(reg));
                    JE_G_SET_RESULT(0);
                    JE_G_RETURN();
                }
                JE_G_BLOCK(wrec, wside, now + timeout);
            }
        }
        uint8_t staging[JE_EP_MSG_MAX];
        je_ep_capdesc caps[JE_ABI_MSG_CAPS_MAX];
        size_t mlen = 0u; uint8_t mcaps = 0u;
        je_ep_recv_lease lease;
        je_ep_status s = je_endpoint_kern_recv_peek_caps(
            vm, index, h, staging,
            je_syscall_recv_staging_cap(cap, sizeof staging),
            &mlen, (uint8_t)hcap, caps, &mcaps, &lease);
        if (s == JE_EP_SHORT_BUFFER) {
            JE_G_SET_STATUS(JE_ABI_H_SHORT_BUFFER);
            JE_G_SET_RESULT((uint64_t)mlen | ((uint64_t)mcaps << 32));
            JE_G_RETURN();
        }
        if (s != JE_EP_OK) {
            JE_G_SET_STATUS(ep_status_to_abi(s));
            JE_G_SET_RESULT(0);
            JE_G_RETURN();
        }
        je_handle minted[JE_ABI_MSG_CAPS_MAX];
        bool was_live[JE_ABI_MSG_CAPS_MAX];
        for (uint8_t c = 0; c < mcaps; ++c) {
            minted[c] = 0u; was_live[c] = false;
        }
        je_abi_status fail = JE_ABI_OK; uint8_t done = 0u;
        for (uint8_t c = 0u; c < mcaps; ++c) {
            if (!je_endpoint_kern_capdesc_live(&caps[c])) {
                minted[c] = 0u; continue;
            }
            je_handle nh = 0u;
            je_ep_status mc = je_endpoint_kern_mint_recv_cap(vm, index, &caps[c], &nh);
            if (mc != JE_EP_OK) { fail = ep_status_to_abi(mc); break; }
            minted[c] = nh; was_live[c] = true; done = (uint8_t)(c + 1u);
        }
        if (fail != JE_ABI_OK) {
            for (uint8_t c = 0u; c < done; ++c)
                if (was_live[c] && minted[c])
                    (void)je_endpoint_kern_close(vm, index, minted[c]);
            JE_G_SET_STATUS(fail); JE_G_SET_RESULT(0); JE_G_RETURN();
        }
        if (mlen != 0u) {
            je_abi_status cs = je_copy_to_user(ubuf, staging, (uint64_t)mlen);
            if (cs != JE_ABI_OK) {
                for (uint8_t c = 0; c < mcaps; ++c)
                    if (was_live[c] && minted[c])
                        (void)je_endpoint_kern_close(vm, index, minted[c]);
                JE_G_SET_STATUS(JE_ABI_INVALID); JE_G_SET_RESULT(0);
                JE_G_RETURN();
            }
        }
        if (mcaps != 0u) {
            uint32_t hout[JE_ABI_MSG_CAPS_MAX];
            for (uint8_t c = 0u; c < mcaps; ++c)
                hout[c] = (uint32_t)minted[c];
            je_abi_status cs = je_copy_to_user(
                hbuf, hout, (uint64_t)mcaps * sizeof(uint32_t));
            if (cs != JE_ABI_OK) {
                for (uint8_t c = 0u; c < mcaps; ++c)
                    if (was_live[c] && minted[c])
                        (void)je_endpoint_kern_close(vm, index, minted[c]);
                JE_G_SET_STATUS(JE_ABI_INVALID); JE_G_SET_RESULT(0);
                JE_G_RETURN();
            }
        }
        je_ep_status cm = je_endpoint_kern_recv_commit(&lease);
        if (cm != JE_EP_OK) {
            JE_G_IPC_FATAL("recv commit mismatch");
        }
        JE_G_NOTE_RECV_CAPS(index, lease.rec, lease.side, mcaps,
                            caps, minted, was_live);
#if JE_TEST_MODE == 13
        for (uint8_t c = 0u; c < mcaps; ++c)
            if (was_live[c] && minted[c])
                je_m4g_record_cap_delivery(vm, index, &caps[c]);
        je_m4g_record_control_receive(vm, index, &lease, staging, mlen, mcaps);
#endif
#if JE_TEST_MODE == 15
        for (uint8_t c = 0u; c < mcaps; ++c)
            if (was_live[c] && minted[c])
                je_m4g_record_cap_delivery(vm, index, &caps[c]);
#endif
        JE_G_SET_STATUS(JE_ABI_OK);
        JE_G_SET_RESULT((uint64_t)mlen | ((uint64_t)mcaps << 32));
        JE_G_RETURN();
    }
#endif
