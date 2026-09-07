//
//  DirectoryManager.swift
//  Folium
//
//  Created by Jarrod Norwell on 11/6/2026.
//

import Foundation.NSURL

actor DirectoryManager {
    private let fileManager: FileManager = .default
    
    var unavailableSystemFiles: [SystemFile] = []
    
    func initializeSystemDirectoriesForInitialLaunch() async throws {
        guard let documentDirectoryURL: URL = await .documentDirectoryURL else {
            return
        }
        
        let subfoldersForSystems: [System : [String : [String : SystemFile]]] = [
            .cherry: [
                "artworks" : [:],
                "games" : [:],
                "system_data": [
                    "bios.col" : SystemFile(path: "system_data",
                                                system: .cherry,
                                                systemFileType: .required,
                                                title: "bios.col")
                ]
            ],
            .cytrus: [
                "cache": [:],
                "cheats": [:],
                "config": [:],
                "dump": [:],
                "external_dlls": [:],
                "games" : [:],
                "icons": [:],
                "load": [:],
                "log": [:],
                "nand": [:],
                "save_states": [:],
                "sdmc": [:],
                "shaders": [:],
                // The 3DS core reads its keys from "sysdata", the same directory name Citra and
                // Azahar use. The app used to call this folder "system_data", which meant a key
                // file imported through the UI was written somewhere the core never looked.
                "sysdata": [
                    "aes_keys.txt" : SystemFile(path: "sysdata",
                                                system: .cytrus,
                                                systemFileType: .required,
                                                title: "aes_keys.txt"),
                    "seeddb.bin" : SystemFile(path: "sysdata",
                                              system: .cytrus,
                                              systemFileType: .optional,
                                              title: "seeddb.bin")
                ]
            ],
            .grape : [
                "artworks" : [:],
                "games" : [:],
                "save_states" : [:],
                "system_data" : [
                    "gba_bios.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .optional,
                                            title: "gba_bios.bin"),
                    "bios7.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .required,
                                            title: "bios7.bin"),
                    "bios9.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .required,
                                            title: "bios9.bin"),
                    "firmware.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .required,
                                            title: "firmware.bin"),
                    "bios7i.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .optional,
                                            title: "bios7i.bin"),
                    "bios9i.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .optional,
                                            title: "bios9i.bin"),
                    "firmwarei.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .optional,
                                            title: "firmwarei.bin"),
                    "nandi.bin" : SystemFile(path: "system_data",
                                            system: .grape,
                                            systemFileType: .optional,
                                            title: "nandi.bin")
                ]
            ],
            .kiwi : [
                "artworks" : [:],
                "games" : [:],
                "save_states" : [:]
            ],
            .mandarine : [
                "artworks" : [:],
                "memory_cards" : [:],
                "games" : [:],
                "save_states" : [:],
                "shaders" : [:],
                "system_data" : [
                    "bios.bin" : SystemFile(path: "system_data",
                                            system: .mandarine,
                                            systemFileType: .required,
                                            title: "bios.bin")
                ]
            ],
            .tomato : [
                "artworks" : [:],
                "games" : [:],
                "save_states" : [:],
                "system_data" : [
                    "bios.bin" : SystemFile(path: "system_data",
                                            system: .tomato,
                                            systemFileType: .required,
                                            title: "bios.bin")
                ]
            ]
        ]
        
        for system in await SystemNames.array {
            let systemDirectoryURL: URL = documentDirectoryURL.appending(component: await system.string)
            try createDirectoryIfNeeded(from: systemDirectoryURL)
            try fixSubfolders(for: systemDirectoryURL, system: system)
            
            if let subfoldersForSystem: [String : [String : SystemFile]] = subfoldersForSystems[system] {
                try loop(subfolders: subfoldersForSystem, for: systemDirectoryURL) { subfolderName in
                    let subfolderDirectoryURL: URL = systemDirectoryURL.appending(component: subfolderName)
                    try createDirectoryIfNeeded(from: subfolderDirectoryURL)
                }
            }
        }
    }
    
    private func createDirectoryIfNeeded(from url: URL) throws {
        if !fileManager.fileExists(atPath: url.path) {
            try fileManager.createDirectory(at: url, withIntermediateDirectories: false)
        }
    }
    
    private func fixSubfolders(for systemDirectoryURL: URL, system: System) throws {
        var replacementSubfolderNames: [String : String] = [
            "memcards" : "memory_cards",
            "roms" : "games",
            "states" : "save_states",
            "sysdata" : "system_data"
        ]

        // The 3DS core hardcodes "sysdata", so for Cytrus the migration runs the other way. Without
        // this the core would create "sysdata" itself and the next launch would try to rename it
        // onto the existing "system_data" and throw, taking the whole setup down with it.
        if system == .cytrus {
            replacementSubfolderNames["sysdata"] = nil
            replacementSubfolderNames["system_data"] = "sysdata"
        }

        for (key, value) in replacementSubfolderNames {
            let oldDirectoryURL: URL = systemDirectoryURL.appending(component: key)
            let newDirectoryURL: URL = systemDirectoryURL.appending(component: value)
            guard fileManager.fileExists(atPath: oldDirectoryURL.path) else {
                continue
            }

            if !fileManager.fileExists(atPath: newDirectoryURL.path) {
                try fileManager.moveItem(at: oldDirectoryURL, to: newDirectoryURL)
                continue
            }

            // Both exist, so move the files across one by one and drop the old directory once it
            // has nothing left in it.
            for name in try fileManager.contentsOfDirectory(atPath: oldDirectoryURL.path) {
                let source: URL = oldDirectoryURL.appending(component: name)
                let destination: URL = newDirectoryURL.appending(component: name)
                if !fileManager.fileExists(atPath: destination.path) {
                    try fileManager.moveItem(at: source, to: destination)
                }
            }

            if try fileManager.contentsOfDirectory(atPath: oldDirectoryURL.path).isEmpty {
                try fileManager.removeItem(at: oldDirectoryURL)
            }
        }
    }
    
    private func loop(subfolders: [String : [String : SystemFile]], for systemDirectoryURL: URL, using handler: (String) throws -> Void) throws {
        for subfolderName in subfolders.keys {
            if let subfiles: [String : SystemFile] = subfolders[subfolderName] {
                for subfile in subfiles.values {
                    if !fileManager.fileExists(atPath: systemDirectoryURL
                        .appending(component: subfile.path)
                        .appending(component: subfile.title).path) {
                        unavailableSystemFiles.append(subfile)
                    }
                }
            }
            
            try handler(subfolderName)
        }
    }
}
