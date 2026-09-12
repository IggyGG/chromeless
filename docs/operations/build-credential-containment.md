# Build bootstrap credential containment

The 12 September native-codec build bootstrap expanded the shared Forgejo
credential into a clone URL while running `bash -euxc`. Shell tracing wrote
that URL into the init-container log. The credential matched the local
`FORGEJO_REPO_TOKEN`; no token value belongs in this document or evidence.

All nine authenticated build and validation manifests now use an ephemeral `GIT_ASKPASS`
helper. The helper reads the credential from its existing secret-provided
environment. Clone arguments and persisted remotes contain only the public
repository URL. Helper creation disables shell tracing, inherited HTTP/Git
trace variables are cleared, and exit cleanup removes the helper on success
and failure. The helper itself contains no literal credential.

The required CI step runs the actual clone/authentication blocks with synthetic
credentials and a Git substitute. Fifty-four cases cover each manifest's success,
shallow-clone refusal and total failure under both bash and sh, verify authentication received the
expected synthetic values, ensure neither username nor password appears in
output, preserve failure status and check helper cleanup. This is source-level
containment evidence, not proof of rotation or a deployed replacement job.

Rotation is still required. The affected Kubernetes secret is
`chromeless-build/forgejo-credentials` (`password`); its value is shared with
other automation through `FORGEJO_REPO_TOKEN`. Identify and update every consumer
before revoking the exposed token through the normal Forgejo credential-owner
path. Revoking the shared token without replacement would interrupt CI and
publication. Verify replacement authentication, old-token denial and the absence
of credential material in a new bootstrap log. Restrict access to the original
cluster log under the operator's evidence/retention procedure; only redacted
copies should enter task archives.

Do not treat this source fix, a redacted local copy, or a successful build as
closure of the credential exposure. The current native-codec build uses the
previous immutable manifest; it is not evidence that this change is live.

A read-only comparison found the exposed value in twelve Opaque Kubernetes
secrets across ten namespaces, including Flux, builders and the automation
receiver. This is a partial consumer inventory; Forgejo action secrets, keychains
and cached process credentials still need reconciliation before rotation.
