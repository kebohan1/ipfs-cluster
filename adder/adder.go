// Package adder implements functionality to add content to IPFS daemons
// managed by the Cluster.
package adder

// #cgo LDFLAGS: -L../pdp -lpdp -lssl -lcrypto
// extern int pdp_tag_file(char *filepath, size_t filepath_len, char *tagfilepath, size_t tagfilepath_len,char* keypath,char* password);
import "C"

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"mime/multipart"
	"os"
	"os/user"
	"path/filepath"
	"strings"

	"github.com/kebohan1/ipfs-cluster/adder/ipfsadd"
	"github.com/kebohan1/ipfs-cluster/api"

	cid "github.com/ipfs/go-cid"
	files "github.com/ipfs/go-ipfs-files"
	ipld "github.com/ipfs/go-ipld-format"
	logging "github.com/ipfs/go-log/v2"
	merkledag "github.com/ipfs/go-merkledag"
	multihash "github.com/multiformats/go-multihash"
)

var logger = logging.Logger("adder")

// ClusterDAGService is an implementation of ipld.DAGService plus a Finalize
// method. ClusterDAGServices can be used to provide Adders with a different
// add implementation.
type ClusterDAGService interface {
	ipld.DAGService
	// Finalize receives the IPFS content root CID as
	// returned by the ipfs adder.
	Finalize(ctx context.Context, ipfsRoot cid.Cid) (cid.Cid, error)
}

// Adder is used to add content to IPFS Cluster using an implementation of
// ClusterDAGService.
type Adder struct {
	ctx    context.Context
	cancel context.CancelFunc

	dgs ClusterDAGService

	params *api.AddParams

	// AddedOutput updates are placed on this channel
	// whenever a block is processed. They contain information
	// about the block, the CID, the Name etc. and are mostly
	// meant to be streamed back to the user.
	output chan *api.AddedOutput
}

// New returns a new Adder with the given ClusterDAGService, add options and a
// channel to send updates during the adding process.
//
// An Adder may only be used once.
func New(ds ClusterDAGService, p *api.AddParams, out chan *api.AddedOutput) *Adder {
	// Discard all progress update output as the caller has not provided
	// a channel for them to listen on.
	if out == nil {
		out = make(chan *api.AddedOutput, 100)
		go func() {
			for range out {
			}
		}()
	}

	return &Adder{
		dgs:    ds,
		params: p,
		output: out,
	}
}

func (a *Adder) setContext(ctx context.Context) {
	if a.ctx == nil { // only allows first context
		ctxc, cancel := context.WithCancel(ctx)
		a.ctx = ctxc
		a.cancel = cancel
	}
}

// FromMultipart adds content from a multipart.Reader. The adder will
// no longer be usable after calling this method.
func (a *Adder) FromMultipart(ctx context.Context, r *multipart.Reader) (cid.Cid, error) {
	logger.Debugf("adding from multipart with params: %+v", a.params)

	f, err := files.NewFileFromPartReader(r, "multipart/form-data")
	if err != nil {
		return cid.Undef, err
	}
	defer f.Close()
	return a.FromFiles(ctx, f)
}

// FromOnepart adds content form a multipart.Reader
func (a *Adder) FromOnepart(ctx context.Context, r *multipart.Reader) (cid.Cid, error) {
	logger.Debugf("adding from multipart with params: %+v", a.params)

	f, err := files.NewFileFromPartReader(r, "multipart/form-data")
	if err != nil {
		return cid.Undef, err
	}
	defer f.Close()
	it := f.Entries()
	for it.Next() {
		name := it.Name()
		file := files.ToFile(it.Node())
		return a.FromFile(ctx, file, name)
	}
	return a.FromFiles(ctx, f)
}

// FromFiles adds content from a files.Directory. The adder will no longer
// be usable after calling this method.
func (a *Adder) FromFiles(ctx context.Context, f files.Directory) (cid.Cid, error) {
	logger.Debug("adding from files")
	a.setContext(ctx)

	if a.ctx.Err() != nil { // don't allow running twice
		return cid.Undef, a.ctx.Err()
	}

	defer a.cancel()
	defer close(a.output)

	ipfsAdder, err := ipfsadd.NewAdder(a.ctx, a.dgs)
	if err != nil {
		logger.Error(err)
		return cid.Undef, err
	}

	ipfsAdder.Trickle = a.params.Layout == "trickle"
	ipfsAdder.RawLeaves = a.params.RawLeaves
	ipfsAdder.Chunker = a.params.Chunker
	ipfsAdder.Out = a.output
	ipfsAdder.Progress = a.params.Progress
	ipfsAdder.NoCopy = a.params.NoCopy

	// Set up prefix
	prefix, err := merkledag.PrefixForCidVersion(a.params.CidVersion)
	if err != nil {
		return cid.Undef, fmt.Errorf("bad CID Version: %s", err)
	}

	hashFunCode, ok := multihash.Names[strings.ToLower(a.params.HashFun)]
	if !ok {
		return cid.Undef, fmt.Errorf("unrecognized hash function: %s", a.params.HashFun)
	}
	prefix.MhType = hashFunCode
	prefix.MhLength = -1
	ipfsAdder.CidBuilder = &prefix

	// setup wrapping
	if a.params.Wrap {
		f = files.NewSliceDirectory(
			[]files.DirEntry{files.FileEntry("", f)},
		)
	}
	usr, _ := user.Current()
	absPath, err := filepath.Abs(usr.HomeDir)
	pdpPath := filepath.Join(absPath, "/.ipfs-cluster/passphrase")
	pdpkeyPath := filepath.Join(absPath, "/.ipfs-cluster/key/")
	pdptagPath := filepath.Join(absPath, "/.ipfs-cluster/tag/")

	logger.Infof("pdpKeyPath:%s", pdpkeyPath)
	logger.Infof("pdpTagPath:%s", pdptagPath)

	passwordFile, _ := os.Open(pdpPath)
	defer passwordFile.Close()
	var password string
	scanner := bufio.NewScanner(passwordFile)
	scanner.Split(bufio.ScanLines)
	for scanner.Scan() {
		password = scanner.Text()
	}

	logger.Infof("pdpPassword:%s", password)

	it := f.Entries()
	var adderRoot ipld.Node
	for it.Next() {
		// In order to set the AddedOutput names right, we use
		// OutputPrefix:
		//
		// When adding a folder, this is the root folder name which is
		// prepended to the addedpaths.  When adding a single file,
		// this is the name of the file which overrides the empty
		// AddedOutput name.
		//
		// After coreunix/add.go was refactored in go-ipfs and we
		// followed suit, it no longer receives the name of the
		// file/folder being added and does not emit AddedOutput
		// events with the right names. We addressed this by adding
		// OutputPrefix to our version. go-ipfs modifies emitted
		// events before sending to user).
		ipfsAdder.OutputPrefix = it.Name()
		name := it.Name()
		size, err := it.Node().Size()
		err_tag := C.pdp_tag_file(C.CString(name), C.ulong(size), C.CString(pdptagPath), C.ulong(len(pdptagPath)), C.CString(pdpkeyPath), C.CString(password))
		if err_tag == 1 {
			logger.Debugf("PDP process Error: %s", it.Name())
			return cid.Undef, a.ctx.Err()
		}
		select {
		case <-a.ctx.Done():
			return cid.Undef, a.ctx.Err()
		default:
			logger.Debugf("ipfsAdder AddFile(%s)", it.Name())

			adderRoot, err = ipfsAdder.AddAllAndPin(it.Node())
			if err != nil {
				logger.Error("error adding to cluster: ", err)
				return cid.Undef, err
			}
		}
	}
	if it.Err() != nil {
		return cid.Undef, it.Err()
	}

	clusterRoot, err := a.dgs.Finalize(a.ctx, adderRoot.Cid())
	if err != nil {
		logger.Error("error finalizing adder:", err)
		return cid.Undef, err
	}
	logger.Infof("%s successfully added to cluster", clusterRoot)
	return clusterRoot, nil
}

// FromFile adds content file. The adder will no longer
// be usable after calling this method.
func (a *Adder) FromFile(ctx context.Context, reader io.Reader, name string) (cid.Cid, error) {
	logger.Debug("add from file")
	a.setContext(ctx)

	if a.ctx.Err() != nil { // don't allow running twice
		return cid.Undef, a.ctx.Err()
	}

	defer a.cancel()
	defer close(a.output)

	ipfsAdder, err := ipfsadd.NewAdder(a.ctx, a.dgs)
	if err != nil {
		logger.Error(err)
		return cid.Undef, err
	}

	ipfsAdder.Trickle = a.params.Layout == "trickle"
	ipfsAdder.RawLeaves = a.params.RawLeaves
	ipfsAdder.Chunker = a.params.Chunker
	ipfsAdder.Out = a.output
	ipfsAdder.Progress = a.params.Progress
	ipfsAdder.NoCopy = a.params.NoCopy

	// Set up prefix
	prefix, err := merkledag.PrefixForCidVersion(a.params.CidVersion)
	if err != nil {
		return cid.Undef, fmt.Errorf("bad CID Version: %s", err)
	}

	hashFunCode, ok := multihash.Names[strings.ToLower(a.params.HashFun)]
	if !ok {
		return cid.Undef, fmt.Errorf("unrecognized hash function: %s", a.params.HashFun)
	}
	prefix.MhType = hashFunCode
	prefix.MhLength = -1
	ipfsAdder.CidBuilder = &prefix

	file := files.NewReaderFile(reader)
	usr, _ := user.Current()
	absPath, err := filepath.Abs(usr.HomeDir)
	pdpPath := filepath.Join(absPath, "/.ipfs-cluster/passphrase")
	pdpkeyPath := filepath.Join(absPath, "/.ipfs-cluster/key/")
	pdptagPath := filepath.Join(absPath, "/.ipfs-cluster/tag/")

	logger.Infof("pdpKeyPath:%s", pdpkeyPath)
	logger.Infof("pdpTagPath:%s", pdptagPath)

	passwordFile, _ := os.Open(pdpPath)
	defer passwordFile.Close()
	var password string
	scanner := bufio.NewScanner(passwordFile)
	scanner.Split(bufio.ScanLines)
	for scanner.Scan() {
		password = scanner.Text()
	}

	logger.Infof("pdpPassword:%s", password)

	size, err := file.Size()
	err_tag := C.pdp_tag_file(C.CString(name), C.ulong(size), C.CString(pdptagPath), C.ulong(len(pdptagPath)), C.CString(pdpkeyPath), C.CString(password))
	if err_tag == 1 {
		logger.Debugf("PDP process Error: %s", name)
		return cid.Undef, a.ctx.Err()
	}
	logger.Debugf("ipfsAdder AddFile(%s)", name)
	var adderRoot ipld.Node
	adderRoot, err = ipfsAdder.AddAllAndPin(file)
	if err != nil {
		logger.Error("error adding to cluster: ", err)
		return cid.Undef, err

	}

	clusterRoot, err := a.dgs.Finalize(a.ctx, adderRoot.Cid())
	if err != nil {
		logger.Error("error finalizing adder:", err)
		return cid.Undef, err
	}
	logger.Infof("%s successfully added to cluster", clusterRoot)
	return clusterRoot, nil
}
