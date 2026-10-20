import * as vscode from "vscode";
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

function serverCommand(): string {
    const configured = vscode.workspace.getConfiguration("strata").get<string>("server.path", "").trim();
    return configured.length > 0 ? configured : "strata-lsp";
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
    const config = vscode.workspace.getConfiguration("strata");
    if (!config.get<boolean>("server.enabled", true)) {
        return;
    }

    const command = serverCommand();
    const serverOptions: ServerOptions = {
        run: { command, transport: TransportKind.stdio },
        debug: { command, transport: TransportKind.stdio },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: "file", language: "strata" },
            { scheme: "untitled", language: "strata" },
        ],
        initializationOptions: {
            importDirs: config.get<string[]>("importDirs", []),
        },
    };

    client = new LanguageClient("strata", "Strata Language Server", serverOptions, clientOptions);

    try {
        await client.start();
    } catch (err) {
        client = undefined;
        vscode.window.showWarningMessage(
            `Strata: could not start '${command}' (${err}). Set "strata.server.path" to the strata-lsp executable.`
        );
    }
}

async function stopClient(): Promise<void> {
    if (client) {
        const running = client;
        client = undefined;
        await running.stop();
    }
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    context.subscriptions.push(
        vscode.commands.registerCommand("strata.restartServer", async () => {
            await stopClient();
            await startClient(context);
        }),
        vscode.workspace.onDidChangeConfiguration(async (e) => {
            if (e.affectsConfiguration("strata.server") || e.affectsConfiguration("strata.importDirs")) {
                await stopClient();
                await startClient(context);
            }
        })
    );

    await startClient(context);
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
