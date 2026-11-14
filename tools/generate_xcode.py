#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the source navigator and shared external-build schemes."""
import hashlib
import plistlib
import subprocess
from pathlib import Path
from xml.etree import ElementTree as ET
ROOT = Path(__file__).resolve().parent.parent
NAME = 'JeiOS'
TASKS = ['check', 'aarch64', 'integration', 'mutants']

def ident(value):
    return hashlib.sha256(value.encode()).hexdigest()[:24].upper()

def main():
    objects = {}
    def add(key, **fields):
        uid = ident(key)
        objects[uid] = fields
        return uid
    configs = []
    for config in ['Debug', 'Release']:
        configs.append(add('config/'+config, isa='XCBuildConfiguration', name=config,
                           buildSettings={'CODE_SIGNING_ALLOWED':'NO'}))
    config_list = add('configs', isa='XCConfigurationList', buildConfigurations=configs,
                     defaultConfigurationIsVisible=0, defaultConfigurationName='Debug')
    # Include tracked files plus the tooling delivered with this project.
    paths = set(subprocess.check_output(['git','ls-files'], cwd=ROOT).decode().splitlines())
    paths.update(['tools/xcode-task.sh','tools/generate_xcode.py','docs/XCODE.md'])
    groups = {'': []}
    for path in sorted(paths):
        parts = Path(path).parts
        if any(x.endswith('.xcodeproj') or x.endswith('.xcworkspace') for x in parts):
            continue
        if not (ROOT/path).is_file():
            continue
        parent = ''
        for part in parts[:-1]:
            child = str(Path(parent)/part)
            if child not in groups:
                groups[child] = []
                groups[parent].append(ident('group/'+child))
            parent = child
        ext = Path(path).suffix
        kind = {'.c':'sourcecode.c.c','.h':'sourcecode.c.h','.S':'sourcecode.asm',
                '.py':'text.script.python','.sh':'text.script.sh','.md':'net.daringfireball.markdown',
                '.json':'text.json'}.get(ext,'text')
        groups[parent].append(add('file/'+path, isa='PBXFileReference', path=parts[-1],
                                  sourceTree='<group>', lastKnownFileType=kind))
    for path, children in groups.items():
        fields = dict(isa='PBXGroup', children=children, sourceTree='<group>')
        if path: fields['path'] = Path(path).name
        add('group/'+path, **fields)
    targets=[]
    project=ROOT/(NAME+'.xcodeproj')
    scheme_dir=project/'xcshareddata/xcschemes'
    scheme_dir.mkdir(parents=True, exist_ok=True)
    for task in TASKS:
        target=add('target/'+task, isa='PBXLegacyTarget', name=task, productName=task,
                   buildConfigurationList=config_list, buildToolPath='/bin/bash',
                   buildArgumentsString='tools/xcode-task.sh '+task,
                   buildWorkingDirectory='$(PROJECT_DIR)', passBuildSettingsInEnvironment=0,
                   buildPhases=[], buildRules=[], dependencies=[])
        targets.append(target)
        scheme=ET.Element('Scheme', LastUpgradeVersion='1500', version='1.3')
        action=ET.SubElement(scheme,'BuildAction',parallelizeBuildables='NO',buildImplicitDependencies='NO')
        entries=ET.SubElement(action,'BuildActionEntries')
        entry=ET.SubElement(entries,'BuildActionEntry',buildForTesting='NO',buildForRunning='YES',
                            buildForProfiling='NO',buildForArchiving='NO',buildForAnalyzing='NO')
        ET.SubElement(entry,'BuildableReference',BuildableIdentifier='primary',BlueprintIdentifier=target,
                      BuildableName=task,BlueprintName=task,ReferencedContainer='container:'+NAME+'.xcodeproj')
        ET.indent(scheme)
        ET.ElementTree(scheme).write(scheme_dir/(task+'.xcscheme'), encoding='UTF-8',xml_declaration=True)
    project_id=add('project',isa='PBXProject',attributes={'LastUpgradeCheck':'1500'},
                   buildConfigurationList=config_list,compatibilityVersion='Xcode 14.0',
                   developmentRegion='en',hasScannedForEncodings=0,knownRegions=['en','Base'],
                   mainGroup=ident('group/'),projectDirPath='',projectRoot='',targets=targets)
    with (project/'project.pbxproj').open('wb') as output:
        plistlib.dump({'archiveVersion':'1','classes':{},'objectVersion':'56',
                      'objects':objects,'rootObject':project_id},output,sort_keys=True)

if __name__ == '__main__': main()
