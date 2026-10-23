status: ready (wine-pe https://github.com/user99672223/win10xbox/actions/runs/39998179308 ; host-uwp https://github.com/user99672223/win10xbox/actions/runs/39998856104 ; uwp-lite https://github.com/user99672223/win10xbox/actions/runs/39998860190 ; ci https://github.com/user99672223/win10xbox/actions/runs/39998017789 rerun)
<!-- handoff
1. Cause (first on-console run, 1.0.0.333): l_intl.nls and c_10000.nls were in neither the package nor the console System32, so RtlInitNlsTables crashed every guest.
2. wine-pe.yml: new step copies wine-pe/wine-src/nls/*.nls to wine-pe/out/nls, fails if l_intl/c_10000/c_1252/locale/sortdefault are missing, uploads artifact `wine-pe-nls`.
3. host-uwp.yml msix job downloads `wine-pe-nls` from the same wine-pe run as the builtins (fails if empty); uwp-lite.yml likewise (non-fatal there).
4. stage.cmake (host-uwp and host-uwp-lite) + CMakeLists (-DNLS_DIR, cache var HOST_UWP_NLS_DIR / UWP_LITE_NLS_DIR) copy them to payload/nls/*.nls.
5. host_stage.cpp / smoke.cpp: StageMachine copies payload\nls into machine\Windows\System32 (sortdefault.nls to Globalization\Sorting) before the host-System32 fallback; log group stage[nls-pkg].
6. nt_nls.cpp reads only the machine folder (no host-System32 fallback), so the runner's System32 cannot serve NLS to a guest. Only the stager has a fallback; the run job's verdict step prints the stage[nls*] lines and warns on stage[nls] copies.
7. Untested here: everything (no Windows/CI run yet). Dispatch wine-pe.yml on the branch first, then host-uwp.yml.
8. CI agent checks: (a) wine-pe artifact `wine-pe-nls` lists l_intl/c_*/locale/norm*/sortdefault (job summary); (b) msix job log "downloaded wine-pe-nls ... N files" and stage.cmake "bundled N NLS files";
   (c) "Assert the package can actually run a guest" and "Verify package contents" pass (payload/nls/*); (d) run job: no "NLS file(s) came from the runner's System32" warning, RESULT: PASS.
9. If wine-src/nls has no *.nls (Wine layout change), the wine-pe step fails with the missing file name; fix the source path there.
-->
