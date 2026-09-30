# Security

Report vulnerabilities through GitHub's **Report a vulnerability** option when available. Otherwise, open an issue with a high-level summary and avoid posting tokens, account data, or exploit details publicly.

The companion sends its token only to the official PIU Scores HTTPS API. Redirects and foreign pagination URLs are refused. The optional overlay listens on loopback, accepts read-only requests, and exposes only gameplay state and website PB. Overlay-only mode performs no score uploads; syncing-only mode creates no listener.

Tokens use Windows encryption for the current user. `settings.json` and `uploads.json` in the local app-data folder are private; share packaged builds rather than that folder. Uploaded or covered score payloads are removed from the queue. Receipts retain event IDs and outcomes for duplicate prevention.

Security fixes target the latest source and release.

Setup runs as the current user, writes only its known installation files, and refuses symbolic links, junctions, and conflicting game layers. Full removal deletes the saved token and queue; partial removal preserves account settings for the remaining component. Build outputs are unsigned unless the distributor signs them.
