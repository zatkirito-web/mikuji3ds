//
//  ImportFileType.swift
//  Folium
//
//  Created by Jarrod Norwell on 26/6/2026.
//

import Foundation

enum ImportFileType {
    case game,
         systemFile,
         saveData
    
    func directory(for system: System?) -> String {
        switch self {
        case .game:
            "games"
        case .systemFile:
            // Cytrus keeps its keys where the 3DS core looks for them.
            system == .cytrus ? "sysdata" : "system_data"
        case .saveData:
            // Where the 3DS core keeps its virtual SD card, which is where game saves live.
            "sdmc"
        }
    }
}
