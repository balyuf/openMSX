// $Id$

#include <cassert>
#include "StringOp.hh"
#include "FileContext.hh"
#include "ConfigException.hh"
#include "xmlx.hh"

namespace openmsx {

// class XMLException

XMLException::XMLException(const string& msg)
	: MSXException(msg)
{
}


// class XMLElement

XMLElement::XMLElement()
	: parent(NULL)
{
}

XMLElement::XMLElement(xmlNodePtr node)
	: parent(NULL)
{
	init(node);
}

XMLElement::XMLElement(const string& name_, const string& data_)
	: name(name_), data(data_), parent(NULL)
{
}

XMLElement::XMLElement(const XMLElement& element)
	: parent(NULL)
{
	*this = element;
}

void XMLElement::init(xmlNodePtr node)
{
	name = (const char*)node->name;
	for (xmlNodePtr x = node->children; x != NULL ; x = x->next) {
		switch (x->type) {
		case XML_TEXT_NODE:
			data += (const char*)x->content;
			break;
		case XML_ELEMENT_NODE:
			addChild(auto_ptr<XMLElement>(new XMLElement(x)));
			break;
		default:
			// ignore
			break;
		}
	}
	for (xmlAttrPtr x = node->properties; x != NULL ; x = x->next) {
		switch (x->type) {
		case XML_ATTRIBUTE_NODE: {
			string name  = (const char*)x->name;
			string value = (const char*)x->children->content;
			addAttribute(name, value);
			break;
		}
		default:
			// ignore
			break;
		}
	}
}

XMLElement::~XMLElement()
{
	for (Children::const_iterator it = children.begin();
	     it != children.end(); ++it) {
		delete *it;
	}
}

XMLElement* XMLElement::getParent()
{
	return parent;
}

const XMLElement* XMLElement::getParent() const
{
	return parent;
}

void XMLElement::addChild(auto_ptr<XMLElement> child)
{
	assert(child.get());
	assert(!child->getParent());
	child->parent = this;
	children.push_back(child.release());
}

void XMLElement::addAttribute(const string& name, const string& value)
{
	assert(attributes.find(name) == attributes.end());
	attributes[name] = value;
}

void XMLElement::getChildren(const string& name, Children& result) const
{
	for (Children::const_iterator it = children.begin();
	     it != children.end(); ++it) {
		if ((*it)->getName() == name) {
			result.push_back(*it);
		}
	}
}

XMLElement* XMLElement::findChild(const string& name)
{
	for (Children::const_iterator it = children.begin();
	     it != children.end(); ++it) {
		if ((*it)->getName() == name) {
			return *it;
		}
	}
	return NULL;
}

const XMLElement* XMLElement::findChild(const string& name) const
{
	return const_cast<XMLElement*>(this)->findChild(name);
}

XMLElement& XMLElement::getChild(const string& name)
{
	XMLElement* elem = findChild(name);
	if (!elem) {
		throw ConfigException("Missing tag \"" + name + "\".");
	}
	return *elem;
}

const XMLElement& XMLElement::getChild(const string& name) const
{
	return const_cast<XMLElement*>(this)->getChild(name);
}

const string& XMLElement::getChildData(const string& name) const
{
	const XMLElement& child = getChild(name);
	return child.getData();
}

string XMLElement::getChildData(const string& name,
                                const string& defaultValue) const
{
	const XMLElement* child = findChild(name);
	return child ? child->getData() : defaultValue;
}

bool XMLElement::getChildDataAsBool(const string& name, bool defaultValue) const
{
	const XMLElement* child = findChild(name);
	return child ? StringOp::stringToBool(child->getData()) : defaultValue;
}

int XMLElement::getChildDataAsInt(const string& name, int defaultValue) const
{
	const XMLElement* child = findChild(name);
	return child ? StringOp::stringToInt(child->getData()) : defaultValue;
}

const string& XMLElement::getAttribute(const string& attName) const
{
	Attributes::const_iterator it = attributes.find(attName);
	if (it == attributes.end()) {
		throw ConfigException("Missing attribute \"" + attName + "\".");
	}
	return it->second;
}

const string XMLElement::getAttribute(const string& attName,
	                              const string defaultValue) const
{
	Attributes::const_iterator it = attributes.find(attName);
	return (it == attributes.end()) ? defaultValue : it->second;
}

bool XMLElement::getAttributeAsBool(const string& attName,
                                    bool defaultValue) const
{
	Attributes::const_iterator it = attributes.find(attName);
	return (it == attributes.end()) ? defaultValue
	                                : StringOp::stringToBool(it->second);
}

int XMLElement::getAttributeAsInt(const string& attName,
                                  int defaultValue) const
{
	Attributes::const_iterator it = attributes.find(attName);
	return (it == attributes.end()) ? defaultValue
	                                : StringOp::stringToInt(it->second);
}

const string& XMLElement::getId() const
{
	const XMLElement* elem = this;
	while (elem) {
		Attributes::const_iterator it = elem->attributes.find("id");
		if (it != elem->attributes.end()) {
			return it->second;
		}
		elem = elem->getParent();
	}
	throw ConfigException("Missing attribute \"id\".");
}

void XMLElement::setFileContext(auto_ptr<FileContext> context_)
{
	context = context_;
}

FileContext& XMLElement::getFileContext() const
{
	return context.get() ? *context.get() : getParent()->getFileContext();
}

const XMLElement& XMLElement::operator=(const XMLElement& element)
{
	if (&element == this) {
		// assign to itself
		return *this;
	}
	name = element.name;
	data = element.data;
	attributes = element.attributes;
	for (Children::const_iterator it = children.begin();
	     it != children.end(); ++it) {
		delete *it;
	}
	children.clear();
	for (Children::const_iterator it = element.children.begin();
	     it != element.children.end(); ++it) {
		addChild(auto_ptr<XMLElement>(new XMLElement(**it)));
	}
	return *this;
}

string XMLElement::dump() const
{
	string result;
	dump(result, 0);
	return result;
}

void XMLElement::dump(string& result, unsigned indentNum) const
{
	string indent(indentNum, ' ');
	result += indent + '<' + getName();
	for (Attributes::const_iterator it = attributes.begin();
	     it != attributes.end(); ++it) {
		result += ' ' + it->first + "=\"" + it->second + '"';
	}
	if (children.empty()) {
		if (data.empty()) {
			result += "/>\n";
		} else {
			result += '>' + data + "</" + getName() + ">\n";
		}
	} else {
		result += ">\n";
		for (Children::const_iterator it = children.begin();
		     it != children.end(); ++it) {
			(*it)->dump(result, indentNum + 2);
		}
		result += indent + "</" + getName() + ">\n";
	}
}


// class XMLDocument

XMLDocument::XMLDocument(const string& filename)
{
	xmlDocPtr doc = xmlParseFile(filename.c_str());
	handleDoc(doc);
}

XMLDocument::XMLDocument(const ostringstream& stream)
{
	xmlDocPtr doc = xmlParseMemory(stream.str().c_str(), stream.str().length());
	handleDoc(doc);
}

void XMLDocument::handleDoc(xmlDocPtr doc)
{
	if (!doc) {
		throw XMLException("Document parsing failed");
	}
	if (!doc->children || !doc->children->name) {
		xmlFreeDoc(doc);
		throw XMLException("Document doesn't contain mandatory root Element");
	}
	init(xmlDocGetRootElement(doc));
	xmlFreeDoc(doc);
}


string XMLEscape(const string& str)
{
	xmlChar* buffer = xmlEncodeEntitiesReentrant(NULL, (const xmlChar*)str.c_str());
	string result = (const char*)buffer;
	// buffer is allocated in C code, soo we free it the C-way:
	if (buffer != NULL) {
		free(buffer);
	}
	return result;
}

} // namespace openmsx
