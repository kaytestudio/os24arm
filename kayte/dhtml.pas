unit dhtml;

interface
uses
  Classes, SysUtils, contnrs,
  JSBindings;

type
  JSContextHandle = Pointer;
  JSValueHandle = Pointer;

  THTMLElement = class
    TagName: String;
    Attributes: TStringList;
    Children: TObjectList;
    InnerText: String;
    JSObjectRef: JSValueHandle; // JavaScript wrapper for this node
    procedure AddChild(Element: THTMLElement);
    constructor Create(ATag: String);
    destructor Destroy; override;
  end;

  THTMLDocument = class
  private
    FRoot: THTMLElement;
    FJSContext: JSContextHandle;
    procedure ExposeElementToJS(Element: THTMLElement; JSContext: JSContextHandle; ParentJSObj: JSValueHandle);
  public
    function LoadFromFile(const Filename: String): Boolean;
    function LoadFromString(const HTML: String): Boolean;
    function SaveToFile(const Filename: String): Boolean;
    function FindElementById(const ID: String): THTMLElement;
    property Root: THTMLElement read FRoot;

    constructor Create;
    destructor Destroy; override;
    procedure ExecuteScript(const Script: String);
    procedure ExposeObjectToJS(const Name: String; Obj: TObject);
    function GetJSDocumentObject: JSValueHandle;
  end;

  { Basic JS event callback }
  TDOMEventCallback = procedure(Sender: TObject) of object;

  { Form input simulation }
  TFormElement = class(THTMLElement)
    Value: String;
    OnChange: TDOMEventCallback;
    procedure TriggerChange;
  end;

implementation

{ THTMLElement }

constructor THTMLElement.Create(ATag: String);
begin
  TagName := ATag;
  Attributes := TStringList.Create;
  Children := TObjectList.Create(True);
  JSObjectRef := nil;
end;

destructor THTMLElement.Destroy;
begin
  // TODO: Release JSObjectRef through the engine before freeing the node.
  Attributes.Free;
  Children.Free;
  inherited Destroy;
end;

procedure THTMLElement.AddChild(Element: THTMLElement);
begin
  Children.Add(Element);
end;

{ TFormElement }

procedure TFormElement.TriggerChange;
begin
  if Assigned(OnChange) then
    OnChange(Self);
  // TODO: Notify JavaScript listeners when event support is added.
end;

{ THTMLDocument }

constructor THTMLDocument.Create;
begin
  inherited Create;
  FJSContext := JSBindings.CreateJSContext;
  FRoot := THTMLElement.Create('html');
  JSBindings.ExposeGlobalObject(FJSContext, 'document', GetJSDocumentObject);
  JSBindings.DefineJSFunction(FJSContext, GetJSDocumentObject, 'getElementById', @JS_getElementById_callback);
end;

destructor THTMLDocument.Destroy;
begin
  JSBindings.ReleaseJSContext(FJSContext);
  FRoot.Free;
  inherited Destroy;
end;

function THTMLDocument.LoadFromFile(const Filename: String): Boolean;
var
  HTML: TStringList;
begin
  Result := False;
  HTML := TStringList.Create;
  try
    HTML.LoadFromFile(Filename);
    Result := LoadFromString(HTML.Text);
  finally
    HTML.Free;
  end;
end;

function THTMLDocument.LoadFromString(const HTML: String): Boolean;
begin
  // TODO: Parse HTML into FRoot before exposing the tree to JavaScript.
  FRoot.Destroy;
  FRoot := THTMLElement.Create('html');
  ExposeElementToJS(FRoot, FJSContext, JSBindings.GetGlobalJSObject(FJSContext));
  Result := True;
end;

procedure THTMLDocument.ExposeElementToJS(Element: THTMLElement; JSContext: JSContextHandle; ParentJSObj: JSValueHandle);
var
  JSObj: JSValueHandle;
  Attr: String;
  ChildElement: THTMLElement;
  i: Integer;
begin
  JSObj := JSBindings.CreateJSObject(JSContext);
  JSBindings.SetJSObjectProperty(JSContext, JSObj, 'tagName', JSBindings.CreateJSString(JSContext, Element.TagName));
  JSBindings.SetJSObjectProperty(JSContext, JSObj, 'innerText', JSBindings.CreateJSString(JSContext, Element.InnerText));

  for Attr in Element.Attributes do
    JSBindings.SetJSObjectProperty(JSContext, JSObj, Attr, JSBindings.CreateJSString(JSContext, Element.Attributes.Values[Attr]));

  JSBindings.SetNativePascalObject(JSContext, JSObj, Element);

  Element.JSObjectRef := JSObj;

  if Assigned(ParentJSObj) then
    JSBindings.AddJSChildElement(JSContext, ParentJSObj, JSObj);

  for i := 0 to Element.Children.Count - 1 do
  begin
    ChildElement := THTMLElement(Element.Children[i]);
    ExposeElementToJS(ChildElement, JSContext, JSObj);
  end;
end;

function THTMLDocument.SaveToFile(const Filename: String): Boolean;
var
  F: TextFile;
begin
  AssignFile(F, Filename);
  Rewrite(F);
  // TODO: Serialize FRoot instead of writing an empty document.
  WriteLn(F, '<html><body></body></html>');
  CloseFile(F);
  Result := True;
end;

function THTMLDocument.FindElementById(const ID: String): THTMLElement;
  function FindRecursive(Elem: THTMLElement): THTMLElement;
  var
    i: Integer;
    Child: THTMLElement;
  begin
    if Elem.Attributes.Values['id'] = ID then
      Exit(Elem);
    for i := 0 to Elem.Children.Count - 1 do
    begin
      Child := THTMLElement(Elem.Children[i]);
      Result := FindRecursive(Child);
      if Assigned(Result) then Exit;
    end;
    Result := nil;
  end;
begin
  Result := FindRecursive(FRoot);
end;

function THTMLDocument.GetJSDocumentObject: JSValueHandle;
begin
  Result := JSBindings.GetGlobalProperty(FJSContext, 'document');
end;

procedure THTMLDocument.ExecuteScript(const Script: String);
begin
  JSBindings.EvaluateJSCode(FJSContext, Script);
end;

procedure THTMLDocument.ExposeObjectToJS(const Name: String; Obj: TObject);
begin
  JSBindings.ExposePascalObject(FJSContext, Name, Obj);
end;

// JavaScript callback for document.getElementById.
procedure JS_getElementById_callback(JSContext: JSContextHandle; JSThis: JSValueHandle;
  JSArgs: array of JSValueHandle; ArgCount: Integer; var JSResult: JSValueHandle); cdecl;
var
  Doc: THTMLDocument;
  ElementID: String;
  FoundElement: THTMLElement;
begin
  JSResult := JSBindings.CreateJSUndefined(JSContext);
  Doc := MainForm.HTMLDoc;

  if (Doc <> nil) and (ArgCount >= 1) and JSBindings.IsJSString(JSArgs[0]) then
  begin
    ElementID := JSBindings.JSValueToString(JSContext, JSArgs[0]);
    FoundElement := Doc.FindElementById(ElementID);
    if Assigned(FoundElement) then
      JSResult := FoundElement.JSObjectRef;
  end;
end;

end.
