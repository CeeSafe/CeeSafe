// SPDX-License-Identifier: MIT
//! The CeeSafe extension for Zed. Highlighting is the grammar's and the queries' (languages/ceesafe); everything else
//! comes from `ceesafec lsp` (spec/tooling.md §2), which this starts: `ceesafec` from the `PATH`, or the command that
//! Zed's settings give for the server `ceesafec`:
//!
//! ```json
//! { "lsp": { "ceesafec": { "binary": { "path": "/opt/ceesafe/bin/ceesafec", "arguments": ["lsp"] } } } }
//! ```

use zed_extension_api::{self as zed, LanguageServerId, Result, settings::LspSettings};

struct CeeSafeExtension;

impl zed::Extension for CeeSafeExtension {
    fn new() -> Self {
        CeeSafeExtension
    }

    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let binary = LspSettings::for_worktree(language_server_id.as_ref(), worktree)
            .ok()
            .and_then(|settings| settings.binary);
        let configured = binary.as_ref().and_then(|b| b.path.clone());
        let command = match configured.or_else(|| worktree.which("ceesafec")) {
            Some(path) => path,
            None => {
                return Err("ceesafec is not on the PATH: install CeeSafe \
                            (https://ceesafe.github.io/CeeSafe/guide/installation), or set \
                            lsp.ceesafec.binary.path in Zed's settings"
                    .to_string());
            }
        };
        let args = binary
            .and_then(|b| b.arguments)
            .unwrap_or_else(|| vec!["lsp".to_string()]);
        Ok(zed::Command {
            command,
            args,
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(CeeSafeExtension);
