//
// Created by lesaha on 22.10.15.
//

#include <stdio.h>
#include <log.h>
#include <string.h>
#include <fat32_structures.h>
#include <malloc.h>
#include <fs_driver.h>

FSState * createFSState(char *fs_mmap, ssize_t mmap_size, char *path, BootRecord *bR){
    FSState* fsState = (FSState*) malloc(sizeof(FSState));
    fsState->bR = bR;
    fsState->currPath = "/";//currently not implemented
    fsState->mmap_size = mmap_size;
    fsState->fs_mmap = fs_mmap;
    fsState->FAT = (uint32_t*) ( (char*) bR + bR->reserved_sectors* bR->bytes_per_sector);
    fsState->cluster_size = bR->bytes_per_sector * bR->sectors_per_cluster;
    fsState->virtualRootDir.starting_cluster_hw = bR->cluster_number_of_the_root_directory>>16;
    fsState->virtualRootDir.starting_cluster_lw = bR->cluster_number_of_the_root_directory & 0xFFFF;
    fsState->currDir = &fsState->virtualRootDir;
    return fsState;
}
void destroyFSState(FSState* fsState){
    free(fsState);
}

DirectoryEntry *changeDirectory(FSState* fsState, char *path) {
    DirectoryEntry *dir = getPtrToDirectory(fsState, path, NULL);
    if (dir != NULL){
        fsState->currDir = dir;
    }
    return dir;
}

DirectoryEntry *getPtrToDirectory(FSState* fsState, char *path, DirectoryEntry*directory) {
    if (path[0]=='/' ) {// calculate from root
        while (path[0] == '/') {
            path++;
        }
        directory = &fsState->virtualRootDir;
        if ( strlen(path)==0){
            return directory;
        }
    }
    if (directory == NULL){
        directory = fsState->currDir;
        if( strlen(path)==0){
            return directory;
        }
    }

    char first_level_file[FILE_PATH_MAX_LEN];
    bzero(first_level_file, FILE_PATH_MAX_LEN);
    int i = 0;
    while( path[0] !='/' && path[0] != 0 && i < FILE_PATH_MAX_LEN){
        first_level_file[i] = path[0];
        path++;
        i++;
    }
    while (path[0]=='/'){
        path++;
    }
    DirectoryEntry* next_dir = getFileWithNameInDirectory(fsState, directory, first_level_file);
    if (  next_dir!= NULL && next_dir->starting_cluster_lw == 0 &&  next_dir->starting_cluster_hw == 0) next_dir = &fsState->virtualRootDir;
    if ( next_dir!= NULL){ // directory ( file) was not found
        if ( strlen(path) == 0){ // if it is last part of the path.
            return next_dir;
        }
        return getPtrToDirectory( fsState, path, next_dir );//if not
    }
    return NULL;
}
DirectoryEntry *getInnerDirectories(FSState* fsState, DirectoryEntry *directories) {
    uint32_t next_dir_cluster_number = directories->starting_cluster_hw;
    next_dir_cluster_number <<=16;
    next_dir_cluster_number += directories->starting_cluster_lw;
    return (DirectoryEntry*) getPtrToFile(fsState, next_dir_cluster_number);
}

DirectoryEntry *getPtrToRootDirectory(FSState* fsState) {
     return (DirectoryEntry* )getPtrToFile(fsState, fsState->bR->cluster_number_of_the_root_directory);
}

ssize_t getNextCluster( FSState* fsState, uint32_t cluster_number){
    ssize_t next_cluster_number = fsState->FAT[cluster_number];
    if ( next_cluster_number >= 0xFFFFFFF){
        return 0;
    }
}

char *getPtrToFile( FSState* fsState, uint32_t cluster_number) {
    BootRecord *bR = fsState->bR;
    return ((char*)bR) +
           (bR->reserved_sectors +
            bR->number_of_copies_of_fat * bR->number_of_sectors_per_fat +
            (cluster_number - 2) * bR->sectors_per_cluster
           ) * bR->bytes_per_sector;
}

DirectoryIterator* createDirectoryIterator(FSState* fsState, DirectoryEntry* dir){
    DirectoryIterator* dirIter = (DirectoryIterator*)malloc(sizeof(DirectoryIterator));
    dirIter->fsState = fsState;
    dirIter->clusterNumber =(dir->starting_cluster_hw << 16) + dir->starting_cluster_lw;
    dirIter->directory = dir;
    dirIter->currentDirectory = ( DirectoryEntry* )getPtrToFile(fsState, dirIter->clusterNumber );
    dirIter->firstDirecrotyInCluster = dirIter->currentDirectory;
    if ( dirIter->currentDirectory != NULL){
        return dirIter;
    }
    return NULL;
}

void destroyDirectoryIterator(DirectoryIterator* dirIter){
    if ( dirIter != NULL){
        free( dirIter);
    }
}

DirectoryEntry* getNextDir(DirectoryIterator* dirIter){
    if ( dirIter==NULL || dirIter->currentDirectory==NULL){
        return NULL;
    }
    while( 1 ){
        if (  ( (char*) (dirIter->currentDirectory +1) - (char*) (dirIter->firstDirecrotyInCluster ) ) >= dirIter->fsState->cluster_size ){
            dirIter->clusterNumber = getNextCluster(dirIter->fsState, dirIter->clusterNumber);
            if ( dirIter->clusterNumber == 0 ){
                dirIter->currentDirectory = NULL;
                return NULL;
            }
            dirIter->currentDirectory = ( DirectoryEntry* ) getPtrToFile(dirIter->fsState, dirIter->clusterNumber );
            dirIter->firstDirecrotyInCluster = dirIter->currentDirectory;
        } else {
            dirIter->currentDirectory++;
        }
        if ( ((char*)dirIter->currentDirectory)[0]==0){ // end of a directory
            dirIter->currentDirectory = NULL;
            return NULL;
        }
        if ( dirIter->currentDirectory->glags==0x0F){ // copy of file
            continue;
        }
        if ( ((uint8_t*) dirIter->currentDirectory)[0] !=0xE5){
            return dirIter->currentDirectory;
        }
    }
    return NULL;
}

DirectoryEntry *getFileWithNameInDirectory(FSState* fsState, DirectoryEntry* dir, char *name) {
    DirectoryIterator *dirIter = createDirectoryIterator(fsState, dir);

    DirectoryEntry *nextDir = NULL;
    while ((nextDir = getNextDir(dirIter)) != NULL){
        if (compareFileAndDirecrtoryName(nextDir, name) == 0) {
            break;
        }
    }
    destroyDirectoryIterator(dirIter);
    return nextDir;
}

int compareFileAndDirecrtoryName(DirectoryEntry *dir, char *name){
    char* file_name = getFileName( dir);
    int result = strcmp( file_name, name);
    LOGMESG( LOG_INFO, "Reading directory %s", file_name);
    free( file_name);
    return result;
}

char* getFileName( DirectoryEntry* dir){
    int i;
    char fname[FILE_NAME_MAX_LEN+1];
    char fname_extension[FILE_EXTENSIO_MAX_LEN+1];
    bzero(fname, FILE_NAME_MAX_LEN+1);
    bzero(fname_extension, FILE_EXTENSIO_MAX_LEN+1);
    memcpy(fname, dir->fname, FILE_NAME_MAX_LEN);
    memcpy(fname_extension, dir->fname_extension, FILE_EXTENSIO_MAX_LEN);
    for( i =0 ; fname[i]!=' ' && i <= FILE_NAME_MAX_LEN; i++);
    fname[i]=0;
    for( i =0 ; fname_extension[i]!=' ' && i <= FILE_EXTENSIO_MAX_LEN; i++);
    fname_extension[i]=0;
    char* file_name = (char*) malloc(FILE_NAME_MAX_LEN+FILE_EXTENSIO_MAX_LEN+2);
    if ( strlen(fname_extension)==0 ){
        sprintf(file_name, "%s", fname);
    }else{
        sprintf(file_name, "%s.%s", fname, fname_extension);
    }
    return file_name;
}

char *readFile(FSState* fsState, DirectoryEntry *dir) {
    ssize_t size_to_copy = dir->file_size;
    uint32_t cluster_number = (dir->starting_cluster_hw << 16) + dir->starting_cluster_lw;
    char *data = (char*) malloc(size_to_copy);
    char* file_block_ptr = (char*)getInnerDirectories( fsState, dir);
    while (size_to_copy > 0){
        ssize_t offset = data - fsState->fs_mmap;
        ssize_t memory_to_copy = size_to_copy > fsState->cluster_size ? fsState->cluster_size : size_to_copy;
        memcpy( data + dir->file_size - size_to_copy, file_block_ptr, memory_to_copy);
        size_to_copy -= memory_to_copy;
        cluster_number = fsState->FAT[cluster_number];
        file_block_ptr = (char*)getPtrToFile(fsState, cluster_number);
    }
    return data;
}