# Code signing for Anvil releases (SmartScreen, antivirus, options)

Research note for the "code signing" fog item in [`docs/design.md`](../design.md)
(the fog list says "Code signing is fog"; the Manager section says "The exe is not signed, so Windows
SmartScreen may warn on the first start"). Researched 2026-10-26 against primary
sources: Microsoft Learn, the Azure retail price API, SignPath's own pages, the
CA/Browser Forum, CA vendor pages, GitHub docs, and the repositories and sites of
similar loaders. No binary was downloaded or run; signature facts about other
projects come from their CI workflows, CI logs, websites and issues only.

Context checked for this note: `happy-dwarf-team/anvil` is MIT
(`gh api repos/happy-dwarf-team/anvil/license` → `MIT`), was created 2026-10-14,
has 0 stars and 654 release downloads in total (2026-10-26), and its release
workflow runs on `ubuntu-24.04` with `actions/attest-build-provenance`. The latest
release v1.5.0 has `anvil-windows-x64.zip`, `AnvilManager.exe`,
`anvil-sdk-windows-x64.zip` and `SHA256SUMS.txt`.

Sections: **verified facts** (each with its source), **inferences** (reasoning
from the facts, marked as such), **options for Anvil**, and **open** questions.

## Question

Can and should Anvil sign `version.dll`, the plugin DLLs, `AnvilManager.exe` and
`anvil.exe`; with what (Azure Artifact Signing, SignPath Foundation, an OV/EV
certificate from a CA); what does each cost and require when the publisher is one
person in Poland with no registered company; and what would signing change for
SmartScreen and antivirus false positives?

## Answer

- **Azure Trusted Signing is now "Artifact Signing"** and generally available
  (rename and GA in January 2026). It costs USD 9.99/month (Basic). Public Trust
  certificates are available to **organizations in the EU** (Poland included), but
  **individual developers must be in the US or Canada**. So the maintainer cannot
  use it as a private person; a registered legal entity in Poland could. The
  certificate shows the validated legal name; certificates live 72 hours and must
  be timestamped. There is an official GitHub Action with OIDC, Windows runners only.
- **SignPath Foundation** signs OSS for free, from GitHub Actions on GitHub-hosted
  runners, with manual approval of every release. **The publisher shown to users is
  "SignPath Foundation", not Anvil.** Terms forbid hacking tools and software that
  circumvents security measures, and require a "verifiable reputation" for
  executables. It has accepted similar tools: OptiScaler (installed as a renamed
  `version.dll`/`dxgi.dll` proxy) and me3 (a FromSoftware mod loader). Anvil is
  12 days old with 0 stars, which may fail the reputation bar.
- **OV/EV certificates**: since 2023-06-01 keys must be in a hardware module
  (token, HSM or a signing service); since 2026-03-01 a certificate may last at most
  460 days (CA/B Forum ballot CSC-31). Certum's Open Source certificate (Polish CA)
  is EUR 25/year on a card or EUR 49/year in the SimplySign cloud, individuals only,
  name prefixed "Open Source Developer", no commercial distribution; both offers were
  out of stock when checked, and unattended CI use of SimplySign is not documented.
  SSL.com OV is USD 129/year plus eSigner cloud signing from USD 180/year; individual
  (IV) certificates need no company.
- **SmartScreen**: Microsoft now states that **EV no longer bypasses SmartScreen**.
  Reputation is built per file hash and per publisher certificate; signing lets
  reputation carry over to new versions signed with the same identity, unsigned
  files start from zero on every release. SmartScreen checks files from the
  internet that the user runs; a Microsoft PM says it checks only the main file,
  not DLLs. **Smart App Control** (Windows 11), by contrast, checks all code loaded
  by the OS loader, DLLs included, and blocks unknown unsigned code.
- **Antivirus**: Microsoft documents the WDSI portal ("submit as a software
  developer") for false positives. No primary source says signing reduces
  detections of proxy DLLs. Similar loaders (Ultimate ASI Loader, Reloaded-II,
  BepInEx, EU4dll, Special K) all get Defender or other AV flags, and their
  maintainers answer by submitting to WDSI or the vendor.
- **Similar loaders**: Ultimate ASI Loader signs its release DLLs in CI since
  2026-10 with a PFX secret (issuer unknown); OptiScaler and me3 use SignPath
  Foundation; ReShade's site publishes an X.509 thumbprint for its installer;
  Reloaded-II, BepInEx/Doorstop, EU4dll and Special K's GitHub releases have no
  signing step in their workflows.

## Verified facts

### 1. Azure Artifact Signing (formerly Trusted Signing)

1. **Name and GA.** The Azure product page FAQ says "Trusted Signing has been
   rebranded as Artifact Signing", with no functional difference, and that existing
   customers must update references in client tools
   ([azure.microsoft.com/products/artifact-signing](https://azure.microsoft.com/en-us/products/artifact-signing)).
   The Microsoft Tech Community post is titled "Simplifying Code Signing for Windows
   Apps: Artifact Signing (GA)"
   ([techcommunity, post 4482789](https://techcommunity.microsoft.com/blog/microsoft-security-blog/-/4482789));
   its body could not be fetched, but search snippets and DevClass date it
   2026-01-12 ([devclass.com, 2026-01-14](https://www.devclass.com/security/2026/01/14/code-signing-windows-apps-may-be-easier-and-more-secure-with-new-azure-artifact-service/4079554),
   secondary). The Learn docs now live under `/azure/artifact-signing/`
   ([overview, ms.date 2026-01-02](https://learn.microsoft.com/en-us/azure/artifact-signing/overview)).
2. **Eligibility.** The quickstart (ms.date 2026-05-21, updated 2026-10-08) says:
   "Public Trust certificates are available to organizations in the United States,
   Canada, the European Union, the United Kingdom, Australia, New Zealand, Japan,
   South Korea, Singapore, Switzerland, Norway, and Israel. Individual developers
   must be located in the United States or Canada."
   ([quickstart, Prerequisites](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart)).
   A paid subscription is required; free, trial and sponsored subscriptions are not
   supported ([FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)).
   Poland Central is one of the service regions ([quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart)).
3. **"Three years of history".** The current quickstart and FAQ do not mention a
   history requirement. It appears in a Microsoft employee's answer on Microsoft
   Q&A dated 2025-04-03 (during the preview): the service was "limited to new
   customer subscriptions from organizations based in the United States and Canada
   with at least three years of verifiable history", and individual onboarding was
   paused ([Microsoft Q&A 2243504](https://learn.microsoft.com/en-us/answers/questions/2243504/trusted-signing-for-other-countries)).
   Whether it still applies after GA: not found in current docs.
4. **Identity validation.** Organizations give the legal entity name, a website,
   a primary email on a domain owned by the entity, a business identifier and the
   business address; a representative then does individual ID verification.
   Processing "takes from 1 to 20 business days"; documents must be issued within
   the last 12 months; there are three attempts to upload more documents
   ([quickstart](https://learn.microsoft.com/en-us/azure/artifact-signing/quickstart)).
   Individuals validate through Microsoft Entra Verified ID with AU10TIX (government
   ID, face check, Microsoft Authenticator); the Azure billing account must have
   Account Type "Individual" and match the ID; city, state/province and country
   appear on the certificate (same page). Custom CN or O are not supported; CN must
   be the validated legal name ([FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)).
   Identity validation expires and must be renewed (FAQ).
5. **Price.** Basic USD 9.99/month (5,000 signatures, 1 certificate profile of each
   type), Premium USD 99.99/month (100,000 signatures), overage USD 0.005 per
   signature ([Change the account SKU, ms.date 2026-01-06](https://learn.microsoft.com/en-us/azure/artifact-signing/how-to-change-sku);
   same numbers in the [Azure retail price API](https://prices.azure.com/api/retail/prices?$filter=serviceName%20eq%20'Trusted%20Signing'),
   meter name still "Trusted Signing", effective 2024-06-01). Billing is not pro
   rata and starts when the account is created ([FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)).
   The public pricing page renders prices with JavaScript and showed placeholders
   ([pricing page](https://azure.microsoft.com/en-us/pricing/details/artifact-signing/)).
6. **Certificates.** "Artifact Signing certificates are renewed daily and are valid
   for only 72 hours"; timestamping is "critical for a signature to be valid beyond
   the life of the signing certificate"; the TSA is `http://timestamp.acs.microsoft.com`.
   Each certificate profile carries a durable EKU `1.3.6.1.4.1.311.97.…`. Keys are in
   FIPS 140-3 Level 3 HSMs and cannot be exported
   ([certificate management, ms.date 2026-09-30](https://learn.microsoft.com/en-us/azure/artifact-signing/concept-certificate-management)).
   The same page says the Artifact Signing CAs "are scheduled for removal from the
   Common CA Database (CCADB)" and are no longer in the Trusted Root Program, but
   "remain on the Windows Platform Trust List" and Windows continues to trust them.
   No EV certificates, and none planned ([FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)).
7. **Revocation for misuse.** Signing malware or a potentially unwanted application
   (PUA) leads to revocation of that day's certificate and possibly account
   suspension ([certificate management](https://learn.microsoft.com/en-us/azure/artifact-signing/concept-certificate-management),
   [FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)).
8. **GitHub Actions.** `azure/artifact-signing-action@v2`; OIDC through
   `azure/login` with `id-token: write` is recommended; inputs `endpoint`,
   `signing-account-name`, `certificate-profile-name`; "This Action can only be
   executed on Windows runners" (windows-2022/2025, no Arm); files must be
   timestamped to stay valid beyond 3 days
   ([Azure/artifact-signing-action](https://github.com/Azure/artifact-signing-action)).
   OIDC to Azure uses an Entra app with a federated credential and at least one
   trust condition ([GitHub docs, OIDC in Azure](https://docs.github.com/en/actions/how-tos/secure-your-work/security-harden-deployments/oidc-in-azure)).
9. **SmartScreen.** The FAQ: "SmartScreen reputation builds up automatically. The
   prompt stops appearing once the file hash has sufficient download history"
   ([FAQ](https://learn.microsoft.com/en-us/azure/artifact-signing/faq)). The
   SmartScreen developer page lists Artifact Signing with "reputation accumulates
   over time based on download volume and behavior"
   ([SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)).
   Smart App Control docs call it "the preferred way to sign your app"
   ([Sign your app for Smart App Control, ms.date 2026-09-25](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/code-signing-for-smart-app-control)).

### 2. SignPath Foundation

10. **Offer.** Free code signing certificates for open source projects; the key is
    on SignPath's HSM; SignPath verifies that the binary was built from the
    project's repository instead of identifying a person
    ([signpath.org](https://signpath.org/)). The foundation is "currently operated
    by SignPath GmbH" ([signpath.org/about](https://signpath.org/about)).
11. **Publisher name.** "The code signing certificate is issued to SignPath
    Foundation. This means that SignPath Foundation is the publisher of the OSS
    project." ([signpath.org/terms](https://signpath.org/terms); the page says the
    code of conduct "is still in a draft version" and carries no date).
12. **Conditions** (same terms page): OSI-approved license for all components, no
    commercial dual licensing, no proprietary code; actively maintained; already
    released in the form to be signed; functionality documented on the download
    page; for executables "a certain verifiable reputation"; built from source "in
    a verifiable way" (CI configuration counts as source); "Every release needs
    manual approval for signing"; MFA for all team members on SignPath and the
    source repository; defined roles (authors, reviewers, approvers); a "Code
    signing policy" page linked from the home page and download page, naming the
    roles and stating privacy behaviour.
13. **Restrictions** (same page): no malware or PUA; "Software must not include
    features designed to identify or exploit security vulnerabilities or circumvent
    security measures of their execution environment"; "must not modify the user's
    system configuration without proper warnings"; installers need uninstall
    instructions; "We're under no obligation to accept your project, and there is
    no independent arbitration mechanism"; SignPath may "revoke the certificate
    effective immediately or retroactively".
14. **Apply.** [signpath.org/apply](https://signpath.org/apply) is a form; its
    fields did not render for automated reading. Review time: not found.
15. **GitHub Actions.** `signpath/github-action-submit-signing-request@v3` with an
    artifact uploaded by `actions/upload-artifact` v4+; the connector verifies that
    "A build was actually performed by a GitHub workflow"; for OSS projects all jobs
    leading to the signing request must run on GitHub-hosted runners; policies can
    forbid re-runs and require branch rulesets
    ([docs.signpath.io, GitHub](https://docs.signpath.io/trusted-build-systems/github)).
16. **Similar accepted projects.** The project list (about 330 entries) includes
    me3 ("A mod loader for FROMSOFTWARE games"), OptiScaler, REPENTOGON, several
    mod managers and game launchers ([signpath.org/projects](https://signpath.org/projects)).
    OptiScaler's README credits "certificate by SignPath Foundation"
    ([optiscaler/OptiScaler](https://github.com/optiscaler/OptiScaler)), and its
    wiki says the DLL is installed as `dxgi.dll`, `winmm.dll`, `d3d12.dll`,
    `dbghelp.dll`, `version.dll`, `wininet.dll` or `winhttp.dll` next to the game
    ([OptiScaler wiki, Manual Installation](https://github.com/optiscaler/OptiScaler/wiki/Manual-Installation)).
    me3's README credits SignPath and links a `CODE_SIGNING_POLICY.md`
    ([garyttierney/me3](https://github.com/garyttierney/me3)).
17. SignPath's pages say nothing about SmartScreen or antivirus behaviour of the
    shared certificate: not found.

### 3. OV/EV certificates and the CA/B Forum rules

18. **Key protection.** Code Signing Baseline Requirements §6.2.7.4.1: "Effective
    June 1, 2023, Subscriber Private Keys for Code Signing Certificates SHALL be
    protected" in "a Hardware Crypto Module ... certified as conforming to at least
    FIPS 140-2 Level 2 or Common Criteria EAL 4+"; §6.2.7.4.2 lists the accepted
    methods, including a CA-shipped token and a Signing Service
    ([CA/B Forum, Code Signing Requirements](https://cabforum.org/working-groups/code-signing/requirements/),
    current version 3.11, 2026).
19. **Validity.** §6.3.2: "For Code Signing Certificates issued on or after March
    1st, 2026, the validity period MUST NOT exceed 460 days." Ballot CSC-31 passed
    (7 CAs yes including Certum, Sectigo, SSL.com, DigiCert; Microsoft yes) and was
    adopted 2025-11-17 ([ballot CSC-31](https://cabforum.org/2025/11/17/ballot-csc-31-maximum-validity-reduction)).
    Certum and Sectigo apply 459 days from 2026-02-27 and 2026-02-23 respectively
    and turn 2-3 year products into free yearly reissues
    ([Certum, Standard Code Signing in the Cloud](https://shop.certum.eu/standard-code-signing-in-the-cloud.html),
    [Sectigo code signing](https://www.sectigo.com/ssl-certificates-tls/code-signing)).
20. **Individuals vs. legal entities.** The requirements define Subscriber as "A
    natural person or Legal Entity" and define an "Individual Applicant", so non-EV
    certificates can name a person; EV subject categories are Private Organization,
    Government Entity, Business Entity and Non-Commercial Entity, with no natural-person
    category ([CSBR §1.6.1, §3.2.2.2.1.1](https://cabforum.org/working-groups/code-signing/requirements/)).
    The exact individual-verification text in §3.2.3 was not read.
21. **Certum Open Source Code Signing.** "issued only for individuals"; CN gets the
    phrase "Open Source Developer" and O is set to "Open Source Developer"; requires
    ID verification, a utility bill and a public project page that shows the
    subscriber's relationship to the project; revoked if used for "software
    distributed commercially"
    ([Certum, required documents](https://support.certum.eu/en/code-signing-required-documents/)).
    Price from EUR 25 (key on the customer's own cryptoCertum card, card and reader
    not included) ([shop](https://shop.certum.eu/open-source-code-signing-code.html))
    or EUR 49 in the SimplySign cloud, limit 5,000 signatures/month
    ([shop](https://shop.certum.eu/open-source-code-signing-on-simplysign.html)).
    Both showed "out of stock" on 2026-10-26. Validity 1 year
    ([Certum code signing comparison](https://www.certum.eu/en/code-signing-certificates/)).
    Certum's other prices there: Standard from EUR 139, EV from EUR 329; Standard in
    the cloud from EUR 209 ([shop](https://shop.certum.eu/standard-code-signing-in-the-cloud.html)).
    SimplySign Desktop login needs a one-time token from the SimplySign phone app
    ([Certum cloud signing manual](https://files.certum.eu/documents/manual_en/CS-Code_Signing_in_the_Cloud_Signtool_jarsigner_signing.pdf),
    via search snippet); unattended CI use: not found in Certum's docs.
22. **SSL.com.** OV code signing USD 129 for 1 year (USD 96.75/year at 5 years);
    YubiKey token +USD 379; attestation for your own Azure Key Vault Premium or Google
    Cloud HSM USD 75, AWS CloudHSM USD 1,275 ([SSL.com buy page](https://www.ssl.com/certificates/code-signing/buy/)).
    IV certificates are for individual developers ("No business entity needed");
    eSigner cloud signing works from "GitHub Actions, Jenkins, Azure DevOps, GitLab
    CI" ([SSL.com code signing](https://www.ssl.com/certificates/code-signing/)).
    eSigner OV/IV tier 1: USD 20/month for 20 signings or USD 180/year for 240,
    overage USD 1 per signing; EV tier 1: USD 100/month or USD 900/year for 120
    ([eSigner pricing, updated 2026-03-25](https://www.ssl.com/guide/esigner-pricing-for-code-signing/)).
23. **Sectigo.** "starts at $536.25 per year when customers choose the five-year
    option"; shorter terms and EV cost more; delivery on a token or onto your own HSM
    (Luna, YubiKey, Google Cloud KMS, Fortanix, YubiHSM 2, nShield)
    ([Sectigo code signing](https://www.sectigo.com/ssl-certificates-tls/code-signing)).
    A per-year OV price for 1 year was not in the extracted page.
24. **Own cloud HSM.** Azure Key Vault Premium lists "Premium HSM-protected RSA
    2048-bit key" at USD 1 and operations at USD 0.03 per 10,000 (Poland Central)
    ([Azure retail price API, Key Vault](https://prices.azure.com/api/retail/prices?$filter=serviceName%20eq%20'Key%20Vault'%20and%20armRegionName%20eq%20'polandcentral')).
    The meter for 3072-bit keys (the CSBR minimum for RSA) was not in that listing.

### 4. SmartScreen, Mark of the Web, Smart App Control

25. **Signals.** SmartScreen evaluates "Publisher reputation" and "File hash
    reputation"; "Even when signed, a newly created binary could still show a
    SmartScreen warning until its hash or publisher certificate accumulates
    sufficient evidence"; unsigned files start "with zero reputation" for each new
    version, and reputation "cannot transfer from previous versions unless both were
    signed using the same publisher identity"
    ([SmartScreen reputation for Windows app developers, ms.date 2026-05-04](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)).
26. **EV.** Same page: "EV certificates no longer bypass SmartScreen. ... they no
    longer impact SmartScreen behavior. Paying a premium for EV solely to avoid
    SmartScreen warnings is no longer justified." The date of the change is not
    given. Eric Lawrence (Microsoft PM, now on Microsoft Defender, personal blog)
    wrote on 2024-11-15 that "EV certificates are no longer treated specially by
    AppRep" ([textslashplain.com](https://textslashplain.com/2024/11/15/best-practices-for-smartscreen-apprep/)).
27. **Reputation building.** "There is no exact threshold, but it can take several
    weeks and hundreds of clean installs from a wide audience"; there is no
    mechanism to submit a file for SmartScreen review for consumer endpoints;
    "Do not sign potentially unwanted applications ... or the certificate may
    develop negative reputation"; "Use a consistent signing identity"
    ([SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)).
    The SmartScreen overview still offers the WDSI form for wrongly shown warnings
    ([SmartScreen overview, ms.date 2026-04-23](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/)).
28. **What is checked.** SmartScreen checks "downloaded files" and "any files an app
    ... attempts to download and run", and "protects against malicious files from
    the internet", not files on internal shares
    ([SmartScreen overview](https://learn.microsoft.com/en-us/windows/security/operating-system-security/virus-and-threat-protection/microsoft-defender-smartscreen/)).
    Eric Lawrence: SmartScreen is invoked for "any Internet-downloaded executable run
    from the Windows Shell" and "only checks the reputation of the _main_ file that
    the user executes"; Smart App Control "evaluates trust/signatures of all code
    (DLLs, scripts, etc) that is loaded by the Windows OS Loader"
    ([textslashplain.com](https://textslashplain.com/2024/11/15/best-practices-for-smartscreen-apprep/),
    Microsoft employee, not official documentation). No official Microsoft page found
    that says whether SmartScreen evaluates a DLL loaded by a running process.
29. **Mark of the Web on zips.** The mark is a `Zone.Identifier` alternate data
    stream, zone 3 for the internet ([Microsoft 365 Apps, internet macros blocked](https://learn.microsoft.com/en-us/microsoft-365-apps/security/internet-macros-blocked)).
    Raymond Chen (Microsoft, 2011): "Since the ZIP file is on an untrusted source, a
    zone identifier is being applied to the extracted file"
    ([The Old New Thing](https://devblogs.microsoft.com/oldnewthing/20110504-00/?p=10743)).
    `Unblock-File` removes the stream ([Unblock-File](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.utility/unblock-file)).
    A current official Learn page on Explorer's zip propagation: not found.
30. **Smart App Control.** Blocks "Malware, Potentially Unwanted Apps (PUA), and
    unknown, unsigned code"; if Microsoft's app intelligence cannot predict, it
    allows code "signed with a certificate issued by a certificate authority (CA)
    within the Trusted Root Program"
    ([Smart App Control overview, ms.date 2026-09-30](https://learn.microsoft.com/en-us/windows/apps/develop/smart-app-control/overview)).
    The SmartScreen developer page adds that its signature checks "apply to all
    executable files, not just those downloaded from the Internet". When it blocks a
    file that "isn't properly signed", "The functionality that depends on the file
    doesn't work, but the rest of the app may continue to work fine"
    ([Microsoft Support](https://support.microsoft.com/en-us/windows/security/threat-malware-protection/smart-app-control-has-blocked-part-of-this-app)).

### 5. Antivirus false positives

31. **Microsoft's process.** "How do I dispute the detection of my program?
    Submit the file in question as a software developer" at
    [microsoft.com/wdsi/filesubmission](https://www.microsoft.com/wdsi/filesubmission);
    sign-in is required; submissions can be tracked; prevalent files and enterprise
    customers are prioritised
    ([Submit files for analysis, updated 2026-09-10](https://learn.microsoft.com/en-us/defender-xdr/submission-guide)).
    The file-submission sign-in moved to Microsoft Entra ID from 2024-05-20 (same page).
32. **Criteria.** Microsoft's malware/PUA criteria list categories (Trojan,
    Hacktool, tampering, evasion, bundling, ...) and say submissions help "start
    establishing reputation"; they say nothing about digital signatures or about
    DLL proxying ([How Microsoft identifies malware and PUA, ms.date 2026-01-29](https://learn.microsoft.com/en-us/defender-xdr/criteria)).
33. **Signing and detections.** No primary Microsoft or AV-vendor statement found
    that code signing reduces detections of proxy DLLs or DLL search-order hijacking.
    The only primary link between signing and AV found is negative: signing PUA can
    give the certificate negative reputation (fact 27), and Artifact Signing revokes
    certificates used on malware/PUA (fact 7).

### 6. What similar loaders do

| Project | Signed? | How (evidence) | What they tell users about AV/SmartScreen |
|---|---|---|---|
| **Ultimate ASI Loader** (proxy `dinput8.dll`/`version.dll` etc.) | Yes since v10.0.0 (2026-10-21) | CI `tools/sign.ps1` runs `signtool sign /f <PFX from secret CODE_SIGNING_PFX> /tr http://timestamp.digicert.com`; the v10.0.0 release run logs `sign:` for `dinput8.dll`, the `.asi` plugins and `VirtualFileServer.exe` ([sign.ps1](https://github.com/ThirteenAG/Ultimate-ASI-Loader/blob/master/tools/sign.ps1), [run 39613630290](https://github.com/ThirteenAG/Ultimate-ASI-Loader/actions/runs/39613630290)). Issuer and trust of the certificate: not determined. | Owner: "I have no control over your antivirus software. ... If you think it's a false positive - report it to your AV vendor" ([#132](https://github.com/ThirteenAG/Ultimate-ASI-Loader/issues/132), 2025-03-19); points to WDSI ([#82](https://github.com/ThirteenAG/Ultimate-ASI-Loader/issues/82), 2023-12-13; [#178](https://github.com/ThirteenAG/Ultimate-ASI-Loader/issues/178)). |
| **Reloaded-II** | No signing step in its workflows | Latest 1.31.0 (2026-09-23) assets `Setup.exe`, `Release.zip`, ... | Maintainer Sewer56: `!ml` detections "every other month"; "The only alternative is I buy a expensive code signing certificate"; scans each release with Defender and submits false positives ([#376](https://github.com/Reloaded-Project/Reloaded-II/issues/376), 2024-05-08). The launcher's error strings name antivirus as a cause of a missing loader DLL ([en-GB.xaml](https://github.com/Reloaded-Project/Reloaded-II/blob/master/source/Reloaded.Mod.Launcher/Assets/Languages/en-GB.xaml)). |
| **BepInEx** (Doorstop `winhttp.dll` proxy) | No signing step in BepInEx or UnityDoorstop workflows | BepInEx v5.4.23.6 zips (2026-10-23); Doorstop v4.6.0 zips | Contributor: "the best option is to ignore the BepInEx DLLs in the anti-virus program" and get BepInEx only from official sources; sent the DLLs to Norton ([#219](https://github.com/BepInEx/BepInEx/issues/219), 2021). |
| **ReShade** | Installer signed (inferred from the site) | reshade.me lists "X.509 Digital Signature Thumbprint: 589690208A5E52FB96980C4A6698F50ACD47C49F" for ReShade 6.8.0 ([reshade.me](https://reshade.me/)); no GitHub releases; CI workflow has no signing step. Publisher name and CA: not found. | Nothing on the download page. |
| **Special K** | Not determined | GitHub release `SpecialK.7z` (SK_26_10_10), no signing step in workflows; the site also offers a Microsoft Store badge (`get.microsoft.com/installer/download/xpdbzpz0lj8rt3`) ([special-k.info](https://www.special-k.info/)) | Open user report of Defender `Trojan:Win32/Bearfoos.A!ml` on `SpecialK32.dll` ([#301](https://github.com/SpecialKO/SpecialK/issues/301), 2025-09); the old Discourse forum now redirects to Discord, so team statements there could not be read. |
| **SKSE64** | Not determined | 7z archives (and an installer for Classic) from skse.silverlock.org, Nexus, Steam ([skse.silverlock.org](https://skse.silverlock.org/)) | Nothing on the site. |
| **EU4dll** (matanki-saito, also a proxy `version.dll` for EU4) | No signing step in its workflow | Release 93 (2026-03-30), `steam_EU4_fixdll_build_93.zip` | Owner submitted the DLL to WDSI and later found "the zip file is reacting to virus detection, not the contents", re-packed the zip locally ([#266](https://github.com/matanki-saito/EU4dll/issues/266), 2024-11/12); in 2022 suggested the old `version.dll` as a workaround ([#234](https://github.com/matanki-saito/EU4dll/issues/234)). |
| **OptiScaler / me3** | Yes | SignPath Foundation (fact 16) | Not checked. |

## Inferences

These follow from the facts above but were not observed directly.

- **Publisher name for Anvil.** Every publicly trusted option names a validated
  legal person: the maintainer's own name (Artifact Signing individual, which is
  closed to Poland; Certum Open Source as "Open Source Developer, <name>"; SSL.com
  IV), a registered company, or "SignPath Foundation". `happy-dwarf-team` can only
  appear if it becomes a registered legal entity (facts 4, 11, 20, 21).
- **Artifact Signing for this team** requires a legal entity in Poland/EU
  (e.g. a registered company with a business identifier, website and domain email)
  and paying about USD 120/year. Whether a Polish sole proprietorship (JDG, CEIDG)
  counts as an "Organization" is not stated. The action needs a Windows runner, so
  Anvil's Linux release job would need a Windows signing job after the build.
- **SignPath fit.** Anvil meets the license, public CI and GitHub-hosted runner
  conditions. The "circumvent security measures" clause targets security
  boundaries; a proxy DLL that the user installs into their own game folder is the
  same mechanism OptiScaler uses with a SignPath certificate, so it is probably not
  disqualifying, but acceptance is at SignPath's discretion. The likely obstacle is
  "verifiable reputation" for a 12-day-old repository with 0 stars. Signing runs on
  SignPath's side, so the Linux runners can stay. The policy page, MFA and a manual
  approval per release are new release-process steps. The publisher would read
  "SignPath Foundation", which tells users less about who made Anvil.
- **Which Anvil files SmartScreen sees.** `AnvilManager.exe` downloaded alone, the
  SDK's `anvil.exe`, and the Manager inside a zip extracted by Explorer carry the
  Mark of the Web and are checked when the user runs them. `version.dll` and the
  plugin DLLs are loaded by `eu4.exe`, which the user starts from Steam; by the
  Microsoft PM's description SmartScreen does not check them. Smart App Control,
  when enforced, does check them, and unknown unsigned DLLs are the case it blocks,
  so signing the DLLs matters more for Smart App Control than for SmartScreen.
- **Signing does not remove the first warnings.** With a new certificate (and with
  SignPath's shared one) users can still see "unrecognized app" on early downloads;
  the gain is that later releases inherit the publisher's reputation instead of
  starting at zero. At Anvil's download volume (654 in total) reputation could take
  long to build.
- **EV is not worth it here**: it no longer affects SmartScreen, Artifact Signing
  does not issue it, and it is not available to a natural person.
- **Antivirus**: expect `!ml` false positives on the proxy DLL with or without
  signing; the documented remedy is WDSI submission per release (and to other
  vendors). EU4dll's case shows a zip can be flagged while its contents are not.
- **Certum Open Source in CI**: the cloud variant would put the key in SimplySign,
  but every session needs a phone OTP, so unattended GitHub Actions signing is
  doubtful; the card variant is a local, manual step. Either breaks "built and
  signed by GitHub Actions" unless a person signs after the build, which also
  changes what the provenance attestation covers.

## Options for Anvil

Prices are list prices found on 2026-10-26, without VAT.

| Option | Cost per year | Publisher shown to users | Effort to set up | Fits GitHub Actions | Effect on SmartScreen | Effect on AV |
|---|---|---|---|---|---|---|
| **Stay unsigned** (today) | 0 | "Unknown publisher" | None | n/a | Warning on every new `AnvilManager.exe`/`anvil.exe` hash until that hash builds reputation; nothing carries over between releases | None; WDSI submissions per release |
| **SignPath Foundation** | 0 | "SignPath Foundation" | Apply; code signing policy page; MFA; roles; manual approval of every release; acceptance not guaranteed, reputation bar may fail now | Yes: official action, GitHub-hosted runners (Linux is fine), artifact upload | Shared certificate; behaviour of its reputation not documented | No primary evidence of change |
| **Artifact Signing, organization** | about USD 120 (Basic, 9.99/month) + Azure subscription | Legal name of the entity | Needs a registered legal entity in the EU; validation 1-20 business days; Entra app + OIDC; renew validation | Yes: official action, OIDC, Windows runner only | Reputation accumulates by download volume; signing keeps it across releases; Microsoft's preferred option for Smart App Control | No primary evidence of change |
| **Artifact Signing, individual** | about USD 120 | Maintainer's name and city | Not available: individuals must be in the US or Canada | (Yes) | (as above) | (as above) |
| **Certum Open Source** | EUR 25 (own card + reader extra) or EUR 49 (SimplySign cloud); out of stock when checked | "Open Source Developer, <maintainer name>" | ID + utility bill + project page; no commercial distribution; yearly renewal | Card: no (manual local step). Cloud: phone OTP per session, unattended use not documented | Same as any OV certificate; Certum claims it helps build reputation | No primary evidence of change |
| **OV/IV from a CA with cloud signing** (e.g. SSL.com IV/OV + eSigner) | USD 129 + USD 180 (eSigner tier 1, 240 signings) | Maintainer's name (IV) or company (OV) | Validation with ID; eSigner credentials as CI secrets | Yes: eSigner works from GitHub Actions per SSL.com | Same as Artifact Signing, but a 460-day certificate means a new certificate (and possibly new reputation) at least every 15 months | No primary evidence of change |
| **OV/IV on own cloud HSM** (Azure Key Vault Premium or Google Cloud HSM) | Certificate (USD 129 at SSL.com) + attestation USD 75 + a few USD/month HSM key | As above | Most work: HSM, key attestation, signing tool | Yes, with OIDC to the cloud | As above | No primary evidence of change |
| **EV** | Certum from EUR 329; SSL.com eSigner EV from USD 900 | Company only | Requires a company | Cloud variants: yes | No SmartScreen advantage any more | No primary evidence of change |

## Open

- Whether Artifact Signing still requires three years of verifiable history for
  organizations after GA, and whether a Polish sole proprietorship (JDG) passes
  Organization validation. Ask Microsoft Q&A (tag `artifact-signing`) or support.
- Whether SignPath Foundation would accept Anvil now, its review time, and how
  "verifiable reputation" is measured. Ask SignPath before building the pipeline.
- How SmartScreen reputation behaves for the shared SignPath Foundation certificate
  and for Artifact Signing's daily certificates (per profile EKU or per identity);
  Microsoft says only that reputation accumulates.
- Whether Smart App Control trusts Artifact Signing's CAs after their removal from
  the Trusted Root Program (they stay on the Windows Platform Trust List, and
  Microsoft calls Artifact Signing the preferred way, so probably yes; not verified).
- Whether SmartScreen evaluates a DLL loaded by a process: only a Microsoft PM's
  blog says it does not; no official page found.
- Which certificate Ultimate ASI Loader signs with (CA, publisher name). Checking it
  needs the binary's signature, which this note did not download.
- ReShade's publisher name and CA, and whether Special K's and SKSE's binaries are
  signed: not found without inspecting binaries.
- Certum Open Source availability (both offers out of stock on 2026-10-26) and
  whether SimplySign can be used unattended from CI.
- A current one-year OV price at Sectigo direct (only the 5-year per-year price was
  extracted), and the Azure Key Vault meter for RSA-3072 HSM keys.
- Primary statements from AV vendors that signing lowers false positives on proxy
  DLLs: none found.
