gate: cmd=docker_compose_run & order=[style_check,tidy_host,test,build] & rc=0_required & path_filters=absent
verify: runner=docker_compose & custom_cmd=absent & host_toolchain=absent

a1: layout=sysmodule/source/{ldn,bsd,config,network,protocol,p2p} & overlay/source & tests/ & scripts/ & hooks/
a2: wire_format=upstream_owned & padding=load_bearing & edit=absent_without_ryujinx_source_proof
a3: dynamic_alloc=hot_path_absent & buffers=fixed & heap=384KB_expanded & growth=prove_needed_first
a4: ownership=explicit & naked_new_delete=absent_outside_main.cpp_allocator_overrides
a5: shared_state=mutex_guarded & m_network_info=m_shared_mutex_on_read_and_write
a6: byte_order=ryujinx_format_in_ldn_fields & bsd_sockaddr=network_order & double_convert=absent

b1: warnings=errors & families=[Wall,Wextra,Wpedantic,Wconversion,Wsign-conversion,Wshadow,Wold-style-cast]
b2: clang_tidy=.clang-tidy_only & WarningsAsErrors='*' & families=[clang-analyzer,bugprone,cppcoreguidelines,google,hicpp,misc,modernize,performance,portability,readability]
b3: surfaces=[sysmodule,overlay,tests,headers] & runner_checks_flag=absent
b4: virtual_default_args=absent & use=non_virtual_convenience_overloads
b5: using_directive=absent & use=using_declaration|namespace_alias
b6: fixed_width_ints & long|long_long=absent & printf=PRId64_family

c1: bypass=absent & markers=[NOLINT,cppcheck-suppress,NOSONAR,lgtm,pragma_diagnostic,clang_suppress,analyzer_guards,sanitizer_opt_out]
c2: finding -> fix_in_code & suppress=absent & unfixable -> BLOCKED.md(proof)
c3: gate_config_weakened(check_removed|threshold_lowered|file_excluded) -> style_check_fails
c4: tests=weakened|skipped|deleted -> absent & swallowed_exit_status=absent

d1: docs=doxygen & every_public_member_documented & params_returns_complete
d2: line<=80 & format=clang-format & tool=docker_compose_run_style
d3: wiki=memory & comments_language=english & personal_names_hosts=absent

e1: tests=host_native & suites=49 & each_suite=own_target & code_change -> tests_same_change
e2: coverage=gate & threshold=lowered_never
e3: sanitizers=[asan,ubsan,lsan,tsan] & stage=pre-push
e4: fuzz=libFuzzer & targets=[protocol_reassembly,ini_parser] & stage=pre-push
e5: valgrind=not_a_gate & reason=allocator_override_suites(BLOCKED.md)

f1: security=[codeql(security-and-quality),cppcheck,flawfinder,semgrep,zizmor,gitleaks] & stage=pre-push
f2: secrets=never_in_clear & permissions=minimal & input=validated
f3: ci_alert_class(codeql,dependabot,secret_scanning) -> found_locally_before_push

g1: commit=signed_off(-s) & ai_attribution=absent & name_email_hardcoded=absent
g2: order=wiki_first_then_repo & wiki_pages=[Handoff-Changes,Technique-Changelog,User-Changelog,Handoff-Current-State]
g3: flow_files_changed(Makefile,compose,workflows,scripts,hooks) -> Handoff-Dev-Flow_moves
g4: push=owner_agreement_only & hooks=owner_edits_only[hooks/,.claude/hooks/,settings,check-coverage.py,hooks_path.py]
g5: proposals=hooks-proposal/
